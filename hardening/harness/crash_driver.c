// Exercises every malformed sample through the full public decode surface:
// stbi_info, 8-bit, 16-bit, float, and animated-GIF entry points, plus the
// I/O-callback path (not just from_memory). A crash/ASan/UBSan report here
// is a hardening failure; every input must return an error code instead.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../../stb_image.h"

typedef struct { const unsigned char *data; int len, pos; } mem_stream;

static int cb_read(void *user, char *dst, int size)
{
   mem_stream *m = (mem_stream *)user;
   int n = m->len - m->pos;
   if (n > size) n = size;
   if (n > 0) { memcpy(dst, m->data + m->pos, (size_t)n); m->pos += n; }
   return n;
}
static void cb_skip(void *user, int n)
{
   mem_stream *m = (mem_stream *)user;
   if (n < 0) { m->pos = 0; return; }
   m->pos += n;
   if (m->pos > m->len) m->pos = m->len;
}
static int cb_eof(void *user)
{
   mem_stream *m = (mem_stream *)user;
   return m->pos >= m->len;
}

int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: %s file [file...]\n", argv[0]); return 2; }
   int crashes_like = 0, decoded = 0, rejected = 0;
   for (int a = 1; a < argc; ++a) {
      FILE *f = fopen(argv[a], "rb");
      if (!f) { fprintf(stderr, "cannot open %s\n", argv[a]); return 2; }
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (n < 0 || n > 256*1024*1024L) { fclose(f); continue; }
      unsigned char *buf = malloc((size_t)(n ? n : 1));
      if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 2; }
      fclose(f);

      int ix = 0, iy = 0, ic = 0;
      int info_ok = stbi_info_from_memory(buf, (int)n, &ix, &iy, &ic);

      int x8 = 0, y8 = 0, c8 = 0;
      unsigned char *img = stbi_load_from_memory(buf, (int)n, &x8, &y8, &c8, 4);
      int x16 = 0, y16 = 0, c16 = 0;
      stbi_us *img16 = stbi_load_16_from_memory(buf, (int)n, &x16, &y16, &c16, 0);
      int xf = 0, yf = 0, cf = 0;
      float *imgf = stbi_loadf_from_memory(buf, (int)n, &xf, &yf, &cf, 0);

      // animated gif entry point (harmless for non-gif inputs)
      int gx = 0, gy = 0, gz = 0, gc = 0, *delays = NULL;
      unsigned char *gif = stbi_load_gif_from_memory(buf, (int)n, &delays,
                                                     &gx, &gy, &gz, &gc, 4);

      // callback path exercises the streaming buffer/skip logic
      stbi_io_callbacks cbs = { cb_read, cb_skip, cb_eof };
      mem_stream ms = { buf, (int)n, 0 };
      int cx = 1, cy = 2, cc = 3;
      unsigned char *streamed = stbi_load_from_callbacks(&cbs, &ms,
                                                         &cx, &cy, &cc, 3);

      if (img || img16 || imgf || gif || streamed) {
         ++decoded;
         // a successful decode must agree with stbi_info and be well-formed
         if (img && (x8 <= 0 || y8 <= 0 || c8 <= 0 || c8 > 4))
            fprintf(stderr, "WARN bad dims after decode for %s\n", argv[a]);
         if (img && info_ok && (ix != x8 || iy != y8))
            fprintf(stderr, "WARN inconsistent dims for %s\n", argv[a]);
      } else {
         ++rejected;
         if (!stbi_failure_reason()) {
            fprintf(stderr, "WARN no failure reason for %s\n", argv[a]);
            ++crashes_like;
         }
      }
      stbi_image_free(img);
      stbi_image_free(img16);
      stbi_image_free(imgf);
      stbi_image_free(gif);
      stbi_image_free(streamed);
      free(delays);
      free(buf);
   }
   printf("samples=%d rejected=%d decoded=%d missing_reason=%d\n",
          argc - 1, rejected, decoded, crashes_like);
   return crashes_like ? 1 : 0;
}
