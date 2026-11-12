// Decode every input file (8-bit forced RGBA, native 16-bit and float where
// applicable) and dump the raw pixels + a manifest line.
// Used both for baseline/hardened consistency diffs and as the driver for
// the malformed-corpus crash tests.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../../stb_image.h"

int main(int argc, char **argv)
{
   if (argc < 3) { fprintf(stderr, "usage: %s outdir file [file...]\n", argv[0]); return 2; }
   const char *outdir = argv[1];
   int failures = 0;
   for (int a = 2; a < argc; ++a) {
      FILE *f = fopen(argv[a], "rb");
      if (!f) { fprintf(stderr, "open fail %s\n", argv[a]); failures++; continue; }
      fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
      unsigned char *buf = malloc(n ? (size_t)n : 1);
      if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read fail\n"); return 2; }
      fclose(f);

      int x, y, c;
      stbi_uc *img = stbi_load_from_memory(buf, (int)n, &x, &y, &c, 4);
      const char *base = strrchr(argv[a], '/');
      base = base ? base + 1 : argv[a];
      char pix[512];
      if (!img) {
         printf("FAIL\t%s\t%s\toffset=%lld\t%s\n", base,
                stbi_failure_reason() ? stbi_failure_reason() : "?",
                stbi_failure_offset(), stbi_failure_detail());
        failures++;
      } else {
         snprintf(pix, sizeof pix, "%s/%s.rgba8.pix", outdir, base);
         FILE *pf = fopen(pix, "wb");
         if (pf) { fwrite(img, 1, (size_t)x*y*4, pf); fclose(pf); }
         printf("OK\t%s\t%dx%d\tn=%d\tbytes=%zd\t%s\n", base, x, y, c, (size_t)x*y*4, pix);
        stbi_image_free(img);
      }

      // 16-bit path
      int x2, y2, c2;
      stbi_us *img16 = stbi_load_16_from_memory(buf, (int)n, &x2, &y2, &c2, 0);
      if (img16) {
         snprintf(pix, sizeof pix, "%s/%s.n16.pix", outdir, base);
         FILE *pf = fopen(pix, "wb");
         if (pf) { fwrite(img16, 2, (size_t)x2*y2*c2, pf); fclose(pf); }
         printf("OK16\t%s\t%dx%d\tn=%d\tbytes=%zd\t%s\n", base, x2, y2, c2,
                (size_t)x2*y2*c2*2, pix);
        stbi_image_free(img16);
      }

      // float path
      int x3, y3, c3;
      float *imgf = stbi_loadf_from_memory(buf, (int)n, &x3, &y3, &c3, 0);
      if (imgf) {
         snprintf(pix, sizeof pix, "%s/%s.f32.pix", outdir, base);
         FILE *pf = fopen(pix, "wb");
         if (pf) { fwrite(imgf, 4, (size_t)x3*y3*c3, pf); fclose(pf); }
         printf("OKF\t%s\t%dx%d\tn=%d\tbytes=%zd\t%s\n", base, x3, y3, c3,
                (size_t)x3*y3*c3*4, pix);
        stbi_image_free(imgf);
      }
      free(buf);
  }
   return failures ? 1 : 0;
}
