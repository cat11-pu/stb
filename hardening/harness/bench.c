// Decoding throughput benchmark. Decodes every input file N times through
// the 8-bit, 16-bit and float paths and reports total time. Built against
// either the baseline or the hardened header; compare the two binaries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../../stb_image.h"

static double now_sec(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
   int reps = argc > 2 ? atoi(argv[2]) : 200;
   FILE *f = fopen(argv[1], "rb");
   if (!f) { perror("open"); return 2; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   unsigned char *buf = malloc((size_t)n);
   if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("read"); return 2; }
   fclose(f);

   long long pixels = 0;
   double t0 = now_sec();
   for (int r = 0; r < reps; ++r) {
      int x, y, c;
      unsigned char *a = stbi_load_from_memory(buf, (int)n, &x, &y, &c, 4);
      if (a) { pixels += (long long)x * y; stbi_image_free(a); }
      stbi_us *b = stbi_load_16_from_memory(buf, (int)n, &x, &y, &c, 0);
      if (b) { pixels += (long long)x * y; stbi_image_free(b); }
      float *d = stbi_loadf_from_memory(buf, (int)n, &x, &y, &c, 0);
      if (d) { pixels += (long long)x * y; stbi_image_free(d); }
   }
   double dt = now_sec() - t0;
   printf("%s\t%.4f s\t%lld px-decodes\t%.2f Mpx/s\n",
          argv[1], dt, pixels, pixels / dt / 1e6);
   return 0;
}
