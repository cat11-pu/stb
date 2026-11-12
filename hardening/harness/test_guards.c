// Functional tests for the hardening guards:
//  1. allocation cap rejects dimension bombs without oversized malloc
//  2. failure offset/detail are populated on malformed inputs
//  3. cap can be disabled and reconfigured at runtime
//  4. normal-sized valid images still decode under a tight cap
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// instrumented allocator: fail (and record) any request above a threshold
static size_t g_threshold = (size_t)-1, g_seen_max = 0, g_oversize_calls = 0;

static void *guard_malloc(size_t n)
{
   if (n > g_seen_max) g_seen_max = n;
   if (n > g_threshold) { ++g_oversize_calls; return NULL; }
   return malloc(n);
}
static void *guard_realloc(void *p, size_t n)
{
   if (n > g_seen_max) g_seen_max = n;
   if (n > g_threshold) { ++g_oversize_calls; return NULL; }
   return realloc(p, n);
}
#define STBI_MALLOC(sz)    guard_malloc(sz)
#define STBI_REALLOC(p,sz) guard_realloc(p,sz)
#define STBI_FREE(p)       free(p)

#define STB_IMAGE_IMPLEMENTATION
#include "../../stb_image.h"

static unsigned char *read_file(const char *path, long *out_len)
{
   FILE *f = fopen(path, "rb");
   if (!f) return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   unsigned char *b = malloc((size_t)(n ? n : 1));
   if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
   fclose(f);
   *out_len = n;
   return b;
}

int main(void)
{
   int failures = 0;
   const char *bomb = "hardening/corpus/bmp_rgb8.bmp_bomb_h_7fffffff.bin";
   const char *valid = "hardening/valid/rgb8.png";
   const char *trunc = "hardening/corpus/png_rgb8.png_trunc03_8.bin";

   long n;
   unsigned char *bomb_buf = read_file(bomb, &n);
   if (!bomb_buf) { printf("SKIP (build %s first)\n", bomb); return 0; }

   // ---- 1. default 256 MB cap: dimension bomb must be rejected ----------
   g_threshold = (size_t)-1; g_seen_max = 0; g_oversize_calls = 0;
   int x, y, c;
   unsigned char *img = stbi_load_from_memory(bomb_buf, (int)n, &x, &y, &c, 4);
   if (img) { printf("FAIL bomb decoded under default cap\n"); ++failures; stbi_image_free(img); }
   else printf("ok  bomb rejected: %s\n", stbi_failure_detail());
   if (g_seen_max <= 256u*1024*1024)
      printf("ok  largest allocation was %zu bytes (<= 256 MB cap)\n", g_seen_max);
   else { printf("FAIL oversized allocation %zu reached allocator\n", g_seen_max); ++failures; }

   // ---- 2. tight custom cap (1 MB) --------------------------------------
   stbi_set_max_alloc(1024*1024);
   g_seen_max = 0;
   img = stbi_load_from_memory(bomb_buf, (int)n, &x, &y, &c, 4);
   if (img) { printf("FAIL bomb decoded under 1 MB cap\n"); ++failures; stbi_image_free(img); }
   else printf("ok  1 MB cap rejection: %s\n", stbi_failure_detail());
   if (g_seen_max <= 1024*1024)
      printf("ok  no malloc exceeded 1 MB (max seen %zu)\n", g_seen_max);
   else { printf("FAIL allocator saw %zu > 1 MB\n", g_seen_max); ++failures; }

   // ---- 3. valid small image still decodes under the tight cap ----------
   long vn;
   unsigned char *vbuf = read_file(valid, &vn);
   if (vbuf) {
      img = stbi_load_from_memory(vbuf, (int)vn, &x, &y, &c, 4);
      if (img && x == 37 && y == 23) printf("ok  valid image decoded under tight cap (%dx%d)\n", x, y);
      else { printf("FAIL valid image rejected: %s\n", stbi_failure_reason()); ++failures; }
      stbi_image_free(img);
      free(vbuf);
   }

   // ---- 4. cap can be disabled ------------------------------------------
   stbi_set_max_alloc(-1);
   g_threshold = 512u*1024*1024; // system allocator guard only
   img = stbi_load_from_memory(bomb_buf, (int)n, &x, &y, &c, 4);
   // either rejected by integer-overflow/dimension checks, or fails via guard
   if (!img) printf("ok  cap disabled, bomb still rejected by format checks (%s)\n", stbi_failure_reason());
   else { printf("FAIL bomb decoded with cap disabled\n"); ++failures; stbi_image_free(img); }
   stbi_set_max_alloc(0); // restore default

   // ---- 5. failure offset locates the problem ---------------------------
   long bn;
   unsigned char *abomb = read_file("hardening/corpus/hm_png_alloc_bomb.bin", &bn);
   if (abomb) {
      stbi_set_max_alloc(0); // default 256 MB
      g_seen_max = 0;
      img = stbi_load_from_memory(abomb, (int)bn, &x, &y, &c, 4);
      if (img) { printf("FAIL 400 MB PNG decoded past cap\n"); ++failures; stbi_image_free(img); }
      else if (strstr(stbi_failure_reason(), "allocation cap"))
         printf("ok  alloc cap fired for 400 MB PNG: %s\n", stbi_failure_detail());
      else { printf("FAIL unexpected reason for alloc bomb: %s\n", stbi_failure_reason()); ++failures; }
      if (g_seen_max <= 256u*1024*1024)
         printf("ok  allocator never exceeded cap (max seen %zu)\n", g_seen_max);
      else { printf("FAIL allocator saw %zu\n", g_seen_max); ++failures; }
      free(abomb);
   }

   // ---- 6. zlib decompression bomb stays under cap during realloc growth --
   long zn;
   unsigned char *zbomb = read_file("hardening/corpus/hm_png_zlib_bomb.bin", &zn);
   if (zbomb) {
      stbi_set_max_alloc(0);
      g_seen_max = 0;
      img = stbi_load_from_memory(zbomb, (int)zn, &x, &y, &c, 4);
      if (img) { printf("FAIL zlib bomb decoded past cap\n"); ++failures; stbi_image_free(img); }
      else if (strstr(stbi_failure_reason(), "too large"))
         printf("ok  zlib bomb rejected at %.1f MB: %s\n",
                g_seen_max / 1048576.0, stbi_failure_detail());
      else { printf("FAIL unexpected zlib bomb reason: %s\n", stbi_failure_reason()); ++failures; }
      if (g_seen_max <= 256u*1024*1024)
         printf("ok  intermediate buffer never exceeded cap\n");
      else { printf("FAIL intermediate alloc %zu\n", g_seen_max); ++failures; }
      free(zbomb);
   }

   // ---- 7. failure offset locates the problem ---------------------------
   long tn;
   unsigned char *tbuf = read_file(trunc, &tn);
   if (tbuf) {
      img = stbi_load_from_memory(tbuf, (int)tn, &x, &y, &c, 4);
      if (img) { printf("FAIL truncated sample decoded\n"); ++failures; stbi_image_free(img); }
      else {
         long long off = stbi_failure_offset();
         if (off >= 0 && off <= tn)
            printf("ok  failure located at offset %lld of %ld: %s\n",
                   off, tn, stbi_failure_detail());
         else { printf("FAIL bad failure offset %lld\n", off); ++failures; }
      }
      free(tbuf);
   }
   free(bomb_buf);
   printf(failures ? "GUARD TESTS FAILED: %d\n" : "ALL GUARD TESTS PASSED\n", failures);
   return failures ? 1 : 0;
}
