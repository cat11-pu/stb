// Generates a corpus of valid images covering every format/bitdepth/channel
// combination stb_image v2.32 can decode. Output goes to hardening/valid/.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../stb_image_write.h"

static unsigned char px(int x, int y, int c, int w, int h)
{
   // deterministic, non-trivial content (gradients + ramps + pattern)
   switch (c) {
      case 0: return (unsigned char)((x*7 + y*13 + (x*y % 17)) & 0xff);
      case 1: return (unsigned char)((x*29 + y*3) & 0xff);
      case 2: return (unsigned char)(int)(128 + 64*sin((double)x/w*6.28) + 32*cos((double)y/h*6.28)) & 0xff;
      case 3: return (unsigned char)(((x ^ y) * 11 + c*40) & 0xff);
   }
   return 0;
}

static unsigned char *make_rgb(int w, int h, int comp)
{
   unsigned char *d = (unsigned char*)malloc((size_t)w*h*comp);
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
         for (int c = 0; c < comp; ++c)
            d[(y*w+x)*comp+c] = px(x,y,c,w,h);
   return d;
}

static float *make_rgb_f(int w, int h, int comp)
{
   float *d = (float*)malloc(sizeof(float)*(size_t)w*h*comp);
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
         for (int c = 0; c < comp; ++c)
            d[(y*w+x)*comp+c] = px(x,y,c,w,h) / 255.0f * (1.0f + 0.25f*c);
   return d;
}

static void write_pnm(const char *path, int w, int h, int comp, int bit16,
                      const unsigned char *d8, const unsigned short *d16)
{
   FILE *f = fopen(path, "wb");
   if (!f) { perror(path); exit(1); }
   fprintf(f, comp >= 3 ? "P6\n" : "P5\n");
   fprintf(f, "%d %d\n", w, h);
   fprintf(f, bit16 ? "65535\n" : "255\n");
   size_t n = (size_t)w*h*comp;
   if (bit16) {
      for (size_t i = 0; i < n; ++i) {
         unsigned short v = d16[i];
         unsigned char be[2] = { (unsigned char)(v >> 8), (unsigned char)(v & 0xff) };
         fwrite(be, 1, 2, f);
      }
   } else {
      fwrite(d8, 1, n, f);
   }
   fclose(f);
}

// minimal uncompressed PSD writer (8-bit, RGB or grayscale, composited view)
// stb_image accepts RGB (color mode 3) PSDs only; header is 26 bytes:
// '8BPS' + version(u16=1) + 6 reserved + channels(u16) + h(u32) + w(u32)
// + depth(u16=8) + mode(u16=3)
static void write_psd(const char *path, int w, int h, const unsigned char *d)
{
   FILE *f = fopen(path, "wb");
   if (!f) { perror(path); exit(1); }
   int channels = 3, depth = 8, colorMode = 3;
   unsigned char hdr[26] = {
      '8','B','P','S',              // 0..3  signature
      0,1,                          // 4..5  version = 1
      0,0,0,0,0,0,                  // 6..11 reserved
      (unsigned char)(channels>>8), (unsigned char)channels,       // 12..13
      (unsigned char)(h>>24),(unsigned char)(h>>16),               // 14..17 rows
      (unsigned char)(h>>8),(unsigned char)h,
      (unsigned char)(w>>24),(unsigned char)(w>>16),               // 18..21 cols
      (unsigned char)(w>>8),(unsigned char)w,
      (unsigned char)(depth>>8),(unsigned char)depth,              // 22..23
      (unsigned char)(colorMode>>8),(unsigned char)colorMode       // 24..25
   };
   fwrite(hdr, 1, 26, f);
   int zeros[5] = {0,0,0,0,0};
   fwrite(zeros, 4, 5, f);
   fputc(0, f); // raw compression
   size_t plane = (size_t)w*h;
   for (int c = 0; c < channels; ++c)
      for (size_t i = 0; i < plane; ++i)
         fputc(d[i*3 + c], f);
   fclose(f);
}

// minimal static GIF89a (global color table, uncompressed-ish LZW)
static void write_gif(const char *path, int w, int h, const unsigned char *rgba)
{
   FILE *f = fopen(path, "wb");
   if (!f) { perror(path); exit(1); }
   unsigned char palette[256][3];
   for (int i = 0; i < 256; ++i) {
      palette[i][0]=(unsigned char)i;
      palette[i][1]=(unsigned char)(255-i);
      palette[i][2]=(unsigned char)((i*7)&255);
   }
   int n = w*h;
   unsigned char *idx = malloc(n);
   for (int i = 0; i < n; ++i)
      // min code size 2 supports 4 symbols; quantize to 0..3
      idx[i] = (unsigned char)(rgba[i*4] & 0x3);
   fwrite("GIF89a", 1, 6, f);
   unsigned char lsd[7] = {(unsigned char)(w&255),(unsigned char)(w>>8),
                           (unsigned char)(h&255),(unsigned char)(h>>8),
                           0xF7, 0, 0};
   fwrite(lsd, 1, 7, f);
   fwrite(palette, 1, 768, f);
   fputc(0x2C, f);
   unsigned char id[9] = {0,0,0,0,(unsigned char)(w&255),(unsigned char)(w>>8),
                          (unsigned char)(h&255),(unsigned char)(h>>8), 0};
   fwrite(id, 1, 9, f);
   fputc(2, f); // LZW minimum code size
   // canonical GIF LZW: minimum code size 2 -> CC=4, EOI=5, first free=6
   unsigned char *out = malloc((size_t)n*3 + 32); int on = 0, bitbuf = 0, bits = 0;
   int codeSize = 3, nextCode = 6, CC = 4, EOI = 5;
   #define EMIT(code) do { bitbuf |= (code) << bits; bits += codeSize; \
      while (bits >= 8) { out[on++] = (unsigned char)(bitbuf & 255); bitbuf >>= 8; bits -= 8; } } while(0)
   EMIT(CC);
   for (int i = 0; i < n; ++i) {
      EMIT(idx[i] & 0xff);              // literal pixel (value 0..3 here)
      if (nextCode == (1 << codeSize) && codeSize < 12) codeSize++;
      if (nextCode < 4095) nextCode++;
   }
   EMIT(EOI);
   if (bits > 0) { out[on++] = (unsigned char)(bitbuf & 255); bits = 0; }
   #undef EMIT
   int pos = 0;
   while (on - pos >= 255) { fputc(255, f); fwrite(out+pos,1,255,f); pos += 255; }
   if (on > pos) { fputc(on-pos, f); fwrite(out+pos,1,on-pos,f); }
   fputc(0, f); fputc(0x3B, f);
   free(out); free(idx);
   fclose(f);
}

int main(void)
{
   const int W = 37, H = 23;
   unsigned char *d1 = make_rgb(W,H,1);
   unsigned char *d2 = make_rgb(W,H,2);
   unsigned char *d3 = make_rgb(W,H,3);
   unsigned char *d4 = make_rgb(W,H,4);
   unsigned short *d16 = malloc(sizeof(unsigned short)*(size_t)W*H);
   for (int i = 0; i < W*H; ++i) d16[i] = (unsigned short)(d1[i]*257);
   float *f3 = make_rgb_f(W,H,3);
   const char *dir = "hardening/valid";

   char path[256];
   #define P(name) (snprintf(path,sizeof path,"%s/%s",dir,name), path)
   stbi_write_png(P("rgb8.png"),   W,H,3,d3,W*3);
   stbi_write_png(P("rgba8.png"),  W,H,4,d4,W*4);
   stbi_write_png(P("gray8.png"),  W,H,1,d1,W);
   stbi_write_png(P("graya8.png"), W,H,2,d2,W*2);
   stbi_write_bmp(P("rgb8.bmp"),   W,H,3,d3);
   stbi_write_bmp(P("gray8.bmp"),  W,H,1,d1);
   stbi_write_tga(P("rgb8.tga"),   W,H,3,d3);
   stbi_write_tga(P("rgba8.tga"),  W,H,4,d4);
   stbi_write_tga(P("gray8.tga"),  W,H,1,d1);
   stbi_write_jpg(P("rgb8.jpg"),   W,H,3,d3,90);
   stbi_write_jpg(P("gray8.jpg"),  W,H,1,d1,85);
   stbi_write_hdr(P("rgb.hdr"),    W,H,3,f3);
   write_pnm(P("rgb8.ppm"), W,H,3,0,d3,NULL);
   write_pnm(P("gray8.pgm"), W,H,1,0,d1,NULL);
   write_pnm(P("gray16.pgm"), W,H,1,1,NULL,d16);
   write_psd(P("rgb8.psd"), W,H,d3);
   write_gif(P("indexed.gif"), W,H,d4);
   printf("generated valid corpus in %s\n", dir);
   return 0;
}
