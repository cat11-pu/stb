// font_render_verify.c - verification & benchmark for font_render.h
//
// Prints four verification data sets plus a performance measurement:
//   V1  subpixel positioning accuracy (jitter) at small size
//   V2  kerning from font tables, incl. mixed Latin+CJK layout
//   V3  small-size (<12px) antialiasing edge smoothness
//   V4  cross-platform determinism checksum (FNV-1a of a fixed render)
//   PERF  time to render 1000 characters (target: < 50 ms)
//
// Usage: font_render_verify [latin.ttf] [cjk.ttf_or_ttc]
// Exit code 0 = all checks pass.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define FONT_RENDER_IMPLEMENTATION
#include "font_render.h"

static double now_ms(void)
{
#ifdef _WIN32
   static LARGE_INTEGER freq;
   LARGE_INTEGER t;
   if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
#endif
}

static unsigned char *load_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   long size;
   unsigned char *data;
   if (!f) return NULL;
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   data = (unsigned char *)malloc((size_t)size);
   if (data && fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); data = NULL; }
   fclose(f);
   return data;
}

static unsigned long long fnv1a64(const unsigned char *data, size_t len)
{
   unsigned long long h = 1469598103934665603ULL;
   size_t i;
   for (i = 0; i < len; i++) { h ^= data[i]; h *= 1099511628211ULL; }
   return h;
}

static double centroid_x(const unsigned char *buf, int w, int h, int stride)
{
   double sum_a = 0, sum_x = 0;
   int x, y;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++) {
         double a = buf[y * stride + x];
         sum_a += a;
         sum_x += a * x;
      }
   return sum_a > 0 ? sum_x / sum_a : 0.0;
}

typedef struct { int levels; int hard_edges; int max_delta; } aa_stats;

static aa_stats edge_stats(const unsigned char *buf, int w, int h, int stride)
{
   aa_stats s;
   unsigned char seen[256];
   int x, y, i;
   s.levels = 0; s.hard_edges = 0; s.max_delta = 0;
   memset(seen, 0, sizeof(seen));
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++) {
         int a = buf[y * stride + x];
         seen[a] = 1;
         if (x + 1 < w) {
            int d = abs(a - buf[y * stride + x + 1]);
            if (d >= 200) s.hard_edges++;
            if (d > s.max_delta) s.max_delta = d;
         }
      }
   for (i = 0; i < 256; i++) s.levels += seen[i];
   return s;
}

static int utf8_len(const char *s)
{
   int n = 0;
   while (*s) { if ((*s & 0xC0) != 0x80) n++; s++; }
   return n;
}

static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int ceil_div(int a, int b)  { return a >= 0 ? (a + b - 1) / b : -((-a) / b); }

/* Renders a glyph at 8x and box-filters down: an ideal-coverage reference. */
static void render_ref_8x(stbtt_fontinfo *info, float scale, float x, float y_base, int glyph,
                          unsigned char *out, int out_w, int out_h, int out_stride)
{
   const int os = 8;
   float fx = x - floorf(x);
   int ix = (int)floorf(x);
   int iy = (int)floorf(y_base);
   int ow, oh, ox, oy, x0, y0, x1, y1, j, i, sy, sx;
   unsigned char *hi = stbtt_GetGlyphBitmapSubpixel(info, scale * os, scale * os,
                                                    fx * os, 0.0f, glyph, &ow, &oh, &ox, &oy);
   if (!hi) return;
   x0 = floor_div(ox, os);       x1 = ceil_div(ox + ow, os);
   y0 = floor_div(oy, os);       y1 = ceil_div(oy + oh, os);
   for (j = y0; j < y1; j++) {
      int dy = iy + j;
      if (dy < 0 || dy >= out_h) continue;
      for (i = x0; i < x1; i++) {
         int dx = ix + i, sum = 0;
         if (dx < 0 || dx >= out_w) continue;
         for (sy = 0; sy < os; sy++) {
            int hy = j * os + sy - oy;
            if (hy < 0 || hy >= oh) continue;
            for (sx = 0; sx < os; sx++) {
               int hx = i * os + sx - ox;
               if (hx < 0 || hx >= ow) continue;
               sum += hi[hy * ow + hx];
            }
         }
         out[dy * out_stride + dx] = (unsigned char)((sum + (os * os) / 2) / (os * os));
      }
   }
   stbtt_FreeBitmap(hi, info->userdata);
}

#define BUF_W 256
#define BUF_H 64

int main(int argc, char **argv)
{
   const char *latin_path = argc > 1 ? argv[1] : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
   const char *cjk_path   = argc > 2 ? argv[2] : "/mnt/c/Windows/Fonts/msyh.ttc";
   int failures = 0;
   unsigned char *buf = (unsigned char *)calloc(BUF_W * BUF_H, 1);
   unsigned char *latin = load_file(latin_path);
   unsigned char *cjk   = load_file(cjk_path);

   if (!latin) { printf("FATAL: cannot load %s\n", latin_path); return 2; }
   if (!cjk)   { printf("FATAL: cannot load %s\n", cjk_path);   return 2; }

   printf("fonts: latin=%s cjk=%s\n\n", latin_path, cjk_path);

   /* ---------- V1: subpixel positioning ---------- */
   printf("[V1] subpixel positioning, 'A' @10px (phase quantization = 1/8 px)\n");
   {
      fr_font *f = fr_create(latin, 0, 10.0f);
      double base = 0, max_err = 0;
      static const float shifts[4] = { 0.0f, 0.25f, 0.5f, 0.75f };
      int i, pass;
      printf("  requested  measured   error\n");
      for (i = 0; i < 4; i++) {
         double c, measured, err;
         memset(buf, 0, BUF_W * BUF_H);
         fr_render(f, buf, BUF_W, BUF_H, BUF_W, 32.0f + shifts[i], 40.0f, "A");
         c = centroid_x(buf, BUF_W, BUF_H, BUF_W);
         if (i == 0) base = c;
         measured = c - base;
         err = fabs(measured - shifts[i]);
         if (err > max_err) max_err = err;
         printf("  %+8.2f  %+8.3f  %6.3f\n", (double)shifts[i], measured, err);
      }
      pass = max_err <= 0.13;   /* 1/8 px phase + rounding slack */
      printf("  max error: %.3f px (tolerance 0.13) -> %s\n\n", max_err, pass ? "PASS" : "FAIL");
      if (!pass) failures++;
      fr_destroy(f);
   }

   /* ---------- V2: kerning, incl. mixed Latin+CJK ---------- */
   printf("[V2] kerning from font tables\n");
   {
      stbtt_fontinfo info;
      float scale, w_kern, w_plain, mw_kern, mw_plain, end_x;
      static const char *pairs[3] = { "AV", "To", "Ye" };
      int i, nonzero = 0, pass;
      fr_font *f, *fc;
      const char *mixed = "AV\xe4\xb8\xad\xe6\x96\x87To";  /* "AV中文To" */

      stbtt_InitFont(&info, latin, stbtt_GetFontOffsetForIndex(latin, 0));
      scale = stbtt_ScaleForPixelHeight(&info, 20.0f);
      for (i = 0; i < 3; i++) {
         int k = stbtt_GetCodepointKernAdvance(&info, pairs[i][0], pairs[i][1]);
         if (k != 0) nonzero++;
         printf("  kern('%s') = %d units = %.3f px @20px\n", pairs[i], k, k * scale);
      }

      f = fr_create(latin, 0, 20.0f);
      w_kern = fr_measure(f, "AV");
      fr_set_kerning(f, 0);
      w_plain = fr_measure(f, "AV");
      fr_set_kerning(f, 1);
      printf("  width(\"AV\"): kern=%.3f px, no-kern=%.3f px, delta=%.3f px\n",
             w_kern, w_plain, w_kern - w_plain);
      pass = nonzero == 3 &&
             fabsf((w_kern - w_plain) - stbtt_GetCodepointKernAdvance(&info, 'A', 'V') * scale) < 0.01f;
      fr_destroy(f);

      /* mixed Latin+CJK layout: per-glyph pen positions + table consistency */
      fc = fr_create(cjk, 0, 16.0f);
      mw_kern = fr_measure(fc, mixed);
      fr_set_kerning(fc, 0);
      mw_plain = fr_measure(fc, mixed);
      fr_set_kerning(fc, 1);
      printf("  mixed \"AV\\xe4\\xb8\\xad\\xe6\\x96\\x87To\" @16px: kern=%.3f px, no-kern=%.3f px, delta=%.3f px\n",
             mw_kern, mw_plain, mw_kern - mw_plain);
      {
         stbtt_fontinfo cinfo;
         float cscale, pen = 0.0f, kern_sum = 0.0f;
         int prev = 0, monotonic = 1;
         const char *p = mixed;
         stbtt_InitFont(&cinfo, cjk, stbtt_GetFontOffsetForIndex(cjk, 0));
         cscale = stbtt_ScaleForPixelHeight(&cinfo, 16.0f);
         printf("  per-glyph layout:  char   pen-x     adv     kern\n");
         while (*p) {
            unsigned long cp;
            int g, adv, lsb, k = 0;
            if ((*p & 0x80) == 0)      { cp = (unsigned char)*p; p += 1; }
            else if ((*p & 0xE0) == 0xC0) { cp = ((p[0]&0x1F)<<6)|(p[1]&0x3F); p += 2; }
            else                       { cp = ((p[0]&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F); p += 3; }
            g = stbtt_FindGlyphIndex(&cinfo, (int)cp);
            if (prev) k = stbtt_GetGlyphKernAdvance(&cinfo, prev, g);
            stbtt_GetGlyphHMetrics(&cinfo, g, &adv, &lsb);
            pen += k * cscale;
            kern_sum += k * cscale;
            printf("    U+%04lx  glyph=%4d  %7.3f  %7.3f  %+7.3f\n",
                   cp, g, pen, adv * cscale, k * cscale);
            if (pen < 0 || (prev && pen <= 0)) monotonic = 0;
            pen += adv * cscale;
            prev = g;
         }
         printf("  layout total=%.3f px, sum of table kerns=%.3f px, glyphs monotonic=%s\n",
                pen, kern_sum, monotonic ? "yes" : "no");
         /* delta reported by fr_measure must equal the sum of table kerns */
         pass = pass && fabsf((mw_kern - mw_plain) - kern_sum) < 0.01f && monotonic;
      }

      memset(buf, 0, BUF_W * BUF_H);
      end_x = fr_render(fc, buf, BUF_W, BUF_H, BUF_W, 4.0f, 32.0f, mixed);
      printf("  rendered end x = %.3f (expected %.3f)\n", end_x, 4.0f + mw_kern);
      pass = pass && fabsf(end_x - (4.0f + mw_kern)) < 0.01f;
      printf("  -> %s\n\n", pass ? "PASS" : "FAIL");
      if (!pass) failures++;
      fr_destroy(fc);
   }

   /* ---------- V3: small-size antialiasing vs 8x supersampled reference ---------- */
   printf("[V3] small-size antialiasing (<12px), production render vs ideal coverage\n");
   {
      static const struct { int use_cjk; float size; const char *ch; const char *name; } cases[6] = {
         { 0,  8.0f, "A",                 "A  " },
         { 0, 10.0f, "A",                 "A  " },
         { 0, 11.0f, "A",                 "A  " },
         { 1,  8.0f, "\xe4\xb8\xad",      "\xe4\xb8\xad" },
         { 1, 10.0f, "\xe4\xb8\xad",      "\xe4\xb8\xad" },
         { 1, 11.0f, "\xe4\xb8\xad",      "\xe4\xb8\xad" },
      };
      unsigned char *ref = (unsigned char *)calloc(BUF_W * BUF_H, 1);
      int i, pass = 1;
      printf("  glyph  size   gray-levels  hard-edges  max-delta  RMSE-vs-8x  maxdiff\n");
      for (i = 0; i < 6; i++) {
         const unsigned char *font_data = cases[i].use_cjk ? cjk : latin;
         fr_font *f = fr_create(font_data, 0, cases[i].size);
         stbtt_fontinfo info;
         aa_stats st;
         double se = 0;
         int maxdiff = 0, y, x, cp;
         memset(buf, 0, BUF_W * BUF_H);
         memset(ref, 0, BUF_W * BUF_H);
         fr_render(f, buf, BUF_W, BUF_H, BUF_W, 32.37f, 40.0f, cases[i].ch);
         stbtt_InitFont(&info, font_data, stbtt_GetFontOffsetForIndex(font_data, 0));
         cp = cases[i].use_cjk ? 0x4e2d : 'A';
         render_ref_8x(&info, stbtt_ScaleForPixelHeight(&info, cases[i].size),
                       32.37f, 40.0f, stbtt_FindGlyphIndex(&info, cp), ref, BUF_W, BUF_H, BUF_W);
         st = edge_stats(buf, BUF_W, BUF_H, BUF_W);
         for (y = 0; y < BUF_H; y++)
            for (x = 0; x < BUF_W; x++) {
               int d = (int)buf[y * BUF_W + x] - (int)ref[y * BUF_W + x];
               se += (double)d * d;
               if (abs(d) > maxdiff) maxdiff = abs(d);
            }
         {
            double rmse = sqrt(se / (BUF_W * BUF_H));
            printf("  %s     %4.0f      %6d       %5d      %6d     %7.3f     %6d\n",
                   cases[i].name, (double)cases[i].size, st.levels, st.hard_edges,
                   st.max_delta, rmse, maxdiff);
            if (!(rmse < 2.0 && st.levels >= 8)) pass = 0;
         }
         fr_destroy(f);
      }
      free(ref);
      printf("  (RMSE/maxdiff in 0..255 alpha units, tolerance RMSE < 2.0) -> %s\n\n",
             pass ? "PASS" : "FAIL");
      if (!pass) failures++;
   }

   /* ---------- V4: cross-platform determinism checksum ---------- */
   printf("[V4] cross-platform consistency checksum\n");
   {
      fr_font *f = fr_create(cjk, 0, 10.5f);
      memset(buf, 0, BUF_W * BUF_H);
      fr_render(f, buf, BUF_W, BUF_H, BUF_W, 3.37f, 30.0f,
                "Hello, \xe4\xb8\x96\xe7\x95\x8c! AV To 123");  /* "Hello, 世界! AV To 123" */
      printf("  FNV-1a-64 of fixed render @10.5px: 0x%016llx\n", fnv1a64(buf, BUF_W * BUF_H));
      printf("  (compare this value across platforms/compilers)\n\n");
      fr_destroy(f);
   }

   /* ---------- PERF: 1000 characters ---------- */
   printf("[PERF] render 1000 characters @16px (mixed Latin+CJK, kerning on)\n");
   {
      const char *unit = "The quick brown fox jumps over the lazy dog. "
                         "\xe6\x95\x8f\xe6\x8d\xb7\xe7\x9a\x84\xe6\xa3\x95\xe8\x89\xb2"
                         "\xe7\x8b\x90\xe7\x8b\xb8\xe8\xb7\xb3\xe8\xbf\x87\xe6\x87\x92\xe7\x8b\x97\xe3\x80\x82";
      int unit_chars = utf8_len(unit);
      int reps = (1000 + unit_chars - 1) / unit_chars;
      size_t cap = (size_t)reps * strlen(unit) + 1;
      char *text = (char *)malloc(cap);
      int i, n_chars, w, h, pass;
      unsigned char *big;
      fr_font *f;
      double t0, cold, best = 1e30, total = 0, avg;
      const int runs = 10;

      text[0] = 0;
      for (i = 0; i < reps; i++) strcat(text, unit);
      n_chars = utf8_len(text);

      w = n_chars * 20 + 64;
      h = 48;
      big = (unsigned char *)malloc((size_t)w * h);
      f = fr_create(cjk, 0, 16.0f);

      memset(big, 0, (size_t)w * h);
      t0 = now_ms();
      fr_render(f, big, w, h, w, 2.0f, 36.0f, text);
      cold = now_ms() - t0;

      for (i = 0; i < runs; i++) {
         double t1, dt;
         memset(big, 0, (size_t)w * h);
         t1 = now_ms();
         fr_render(f, big, w, h, w, 2.0f, 36.0f, text);
         dt = now_ms() - t1;
         if (dt < best) best = dt;
         total += dt;
      }
      avg = total / runs;
      printf("  characters: %d, cold: %.2f ms, warm best: %.2f ms, warm avg: %.2f ms\n",
             n_chars, cold, best, avg);
      pass = avg < 50.0;
      printf("  target < 50 ms -> %s\n\n", pass ? "PASS" : "FAIL");
      if (!pass) failures++;

      free(big);
      free(text);
      fr_destroy(f);
   }

   printf("%s (%d failure%s)\n", failures ? "OVERALL: FAIL" : "OVERALL: PASS",
          failures, failures == 1 ? "" : "s");

   free(buf);
   free(latin);
   free(cjk);
   return failures ? 1 : 0;
}
