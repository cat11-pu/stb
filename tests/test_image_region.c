// Region/streaming PNG decoder verification.
//
// The synthetic PNGs use uncompressed Deflate blocks so the test controls
// every scanline filter without depending on an external compressor.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef union
{
   size_t size;
   double align;
} rg_alloc_header;

static size_t rg_live_bytes;
static size_t rg_peak_bytes;
static int rg_track_allocations;

static void *rg_malloc(size_t size)
{
   rg_alloc_header *header = (rg_alloc_header *) malloc(sizeof(*header) + size);
   if (!header)
      return NULL;
   header->size = size;
   if (rg_track_allocations) {
      rg_live_bytes += size;
      if (rg_live_bytes > rg_peak_bytes)
         rg_peak_bytes = rg_live_bytes;
   }
   return header + 1;
}

static void rg_free(void *ptr)
{
   rg_alloc_header *header;
   if (!ptr)
      return;
   header = ((rg_alloc_header *) ptr) - 1;
   if (rg_track_allocations)
      rg_live_bytes -= header->size;
   free(header);
}

static void *rg_realloc(void *ptr, size_t size)
{
   rg_alloc_header *header;
   size_t old_size;
   void *next;
   if (!ptr)
      return rg_malloc(size);
   header = ((rg_alloc_header *) ptr) - 1;
   old_size = header->size;
   next = rg_malloc(size);
   if (!next)
      return NULL;
   memcpy(next, ptr, old_size < size ? old_size : size);
   rg_free(ptr);
   return next;
}

#define STBI_MALLOC(sz)          rg_malloc(sz)
#define STBI_REALLOC(p,newsz)    rg_realloc(p,newsz)
#define STBI_REALLOC_SIZED(p,oldsz,newsz) rg_realloc(p,newsz)
#define STBI_FREE(p)             rg_free(p)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

typedef struct
{
   unsigned char *data;
   int len;
   int cap;
} byte_buffer;

static void buffer_reserve(byte_buffer *b, int extra)
{
   if (b->len + extra > b->cap) {
      int cap = b->cap ? b->cap : 256;
      while (cap < b->len + extra)
         cap *= 2;
      b->data = (unsigned char *) realloc(b->data, (size_t) cap);
      if (!b->data) {
         fprintf(stderr, "out of memory building test PNG\n");
         exit(2);
      }
      b->cap = cap;
   }
}

static void buffer_put(byte_buffer *b, const void *data, int len)
{
   buffer_reserve(b, len);
   memcpy(b->data + b->len, data, (size_t) len);
   b->len += len;
}

static void buffer_u8(byte_buffer *b, unsigned value)
{
   unsigned char v = (unsigned char) value;
   buffer_put(b, &v, 1);
}

static void buffer_u16le(byte_buffer *b, unsigned value)
{
   unsigned char v[2];
   v[0] = (unsigned char) value;
   v[1] = (unsigned char) (value >> 8);
   buffer_put(b, v, 2);
}

static void buffer_u32be(byte_buffer *b, uint32_t value)
{
   unsigned char v[4];
   v[0] = (unsigned char) (value >> 24);
   v[1] = (unsigned char) (value >> 16);
   v[2] = (unsigned char) (value >> 8);
   v[3] = (unsigned char) value;
   buffer_put(b, v, 4);
}

static uint32_t crc_table[256];

static void crc_init(void)
{
   uint32_t n, k, c;
   for (n=0; n < 256; ++n) {
      c = n;
      for (k=0; k < 8; ++k)
         c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      crc_table[n] = c;
   }
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *data, int len)
{
   int i;
   for (i=0; i < len; ++i)
      crc = crc_table[(crc ^ data[i]) & 255] ^ (crc >> 8);
   return crc;
}

static void png_chunk(byte_buffer *b, const char type[4],
                      const unsigned char *data, int len)
{
   uint32_t crc = 0xffffffffu;
   buffer_u32be(b, (uint32_t) len);
   buffer_put(b, type, 4);
   if (len)
      buffer_put(b, data, len);
   crc = crc32_update(crc, (const unsigned char *) type, 4);
   crc = crc32_update(crc, data, len);
   buffer_u32be(b, crc ^ 0xffffffffu);
}

static uint32_t adler32_update(uint32_t adler, const unsigned char *data, int len)
{
   uint32_t s1 = adler & 0xffff;
   uint32_t s2 = adler >> 16;
   int i;
   for (i=0; i < len; ++i) {
      s1 = (s1 + data[i]) % 65521;
      s2 = (s2 + s1) % 65521;
   }
   return (s2 << 16) | s1;
}

static void zlib_store(byte_buffer *out, const unsigned char *data, int len)
{
   int at = 0;
   buffer_u8(out, 0x78);
   buffer_u8(out, 0x01);
   do {
      int n = len - at;
      int final;
      if (n > 65535)
         n = 65535;
      final = at + n == len;
      buffer_u8(out, final ? 1 : 0);
      buffer_u16le(out, (unsigned) n);
      buffer_u16le(out, (unsigned) (n ^ 0xffff));
      if (n)
         buffer_put(out, data + at, n);
      at += n;
   } while (at < len);
   buffer_u32be(out, adler32_update(1, data, len));
}

typedef struct
{
   int width;
   int height;
   int depth;
   int color;
   int interlace;
   int palette_n;
   int trns_len;
   unsigned char trns[6];
   int idat_chunk_size;
   int filter_seed;
} png_spec;

static int spec_channels(const png_spec *s)
{
   if (s->color == 3)
      return 1;
   return (s->color & 2 ? 3 : 1) + (s->color & 4 ? 1 : 0);
}

static int spec_row_bytes(const png_spec *s)
{
   return (s->width * spec_channels(s) * s->depth + 7) >> 3;
}

static unsigned sample_value(int x, int y, int component, int depth)
{
   unsigned v = (unsigned) (x * 37 + y * 61 + component * 83 + 17);
   if (depth == 16)
      return (v * 257u + (unsigned) (x * y * 13)) & 0xffffu;
   if (depth == 8)
      return v & 255u;
   return v & ((1u << depth) - 1u);
}

static void make_unfiltered_row(const png_spec *s, int y, unsigned char *row)
{
   int x, c, n = spec_channels(s);
   memset(row, 0, (size_t) spec_row_bytes(s));
   for (x=0; x < s->width; ++x) {
      for (c=0; c < n; ++c) {
         unsigned value = sample_value(x, y, c, s->depth);
         if (s->color == 3)
            value %= (unsigned) s->palette_n;
         if (s->depth == 16) {
            int p = (x * n + c) * 2;
            row[p+0] = (unsigned char) (value >> 8);
            row[p+1] = (unsigned char) value;
         } else if (s->depth == 8) {
            row[x * n + c] = (unsigned char) value;
         } else {
            int bit = x * s->depth;
            int shift = 8 - s->depth - (bit & 7);
            row[bit >> 3] |= (unsigned char) (value << shift);
         }
      }
   }
}

static int paeth(int a, int b, int c)
{
   int p = a + b - c;
   int pa = abs(p - a);
   int pb = abs(p - b);
   int pc = abs(p - c);
   if (pa <= pb && pa <= pc)
      return a;
   if (pb <= pc)
      return b;
   return c;
}

static void filter_row(int filter, const unsigned char *row,
                       const unsigned char *prev, unsigned char *out,
                       int len, int bpp)
{
   int i;
   for (i=0; i < len; ++i) {
      int a = i >= bpp ? row[i-bpp] : 0;
      int b = prev ? prev[i] : 0;
      int c = i >= bpp && prev ? prev[i-bpp] : 0;
      int predictor = 0;
      switch (filter) {
         case 0: predictor = 0; break;
         case 1: predictor = a; break;
         case 2: predictor = b; break;
         case 3: predictor = (a + b) >> 1; break;
         case 4: predictor = paeth(a, b, c); break;
      }
      out[i] = (unsigned char) (row[i] - predictor);
   }
}

static unsigned char *build_png(const png_spec *s, int *out_len)
{
   static const unsigned char signature[8] = { 137,80,78,71,13,10,26,10 };
   byte_buffer png = { 0, 0, 0 };
   byte_buffer filtered = { 0, 0, 0 };
   byte_buffer compressed = { 0, 0, 0 };
   unsigned char ihdr[13];
   unsigned char palette[256*3];
   unsigned char *row, *prev = NULL;
   int y, row_bytes = spec_row_bytes(s);
   int bpp = (spec_channels(s) * s->depth + 7) >> 3;
   int at;
   uint32_t w = (uint32_t) s->width, h = (uint32_t) s->height;

   if (bpp < 1)
      bpp = 1;
   ihdr[0] = (unsigned char) (w >> 24);
   ihdr[1] = (unsigned char) (w >> 16);
   ihdr[2] = (unsigned char) (w >> 8);
   ihdr[3] = (unsigned char) w;
   ihdr[4] = (unsigned char) (h >> 24);
   ihdr[5] = (unsigned char) (h >> 16);
   ihdr[6] = (unsigned char) (h >> 8);
   ihdr[7] = (unsigned char) h;
   ihdr[8] = (unsigned char) s->depth;
   ihdr[9] = (unsigned char) s->color;
   ihdr[10] = 0;
   ihdr[11] = 0;
   ihdr[12] = (unsigned char) s->interlace;

   row = (unsigned char *) malloc((size_t) row_bytes);
   if (!row || row_bytes <= 0)
      exit(2);
   for (y=0; y < s->height; ++y) {
      static const int filters[5] = { 0, 1, 2, 3, 4 };
      int filter = filters[(y + s->filter_seed) % 5];
      make_unfiltered_row(s, y, row);
      buffer_u8(&filtered, (unsigned) filter);
      buffer_reserve(&filtered, row_bytes);
      filter_row(filter, row, prev, filtered.data + filtered.len, row_bytes, bpp);
      filtered.len += row_bytes;
      {
         unsigned char *swap = prev;
         prev = row;
         row = swap ? swap : (unsigned char *) malloc((size_t) row_bytes);
         if (!row)
            exit(2);
      }
   }
   free(prev);
   free(row);

   buffer_put(&png, signature, 8);
   png_chunk(&png, "IHDR", ihdr, 13);
   if (s->color == 3) {
      int i;
      for (i=0; i < s->palette_n; ++i) {
         palette[i*3+0] = (unsigned char) (i * 29 + 3);
         palette[i*3+1] = (unsigned char) (i * 47 + 11);
         palette[i*3+2] = (unsigned char) (i * 71 + 19);
      }
      png_chunk(&png, "PLTE", palette, s->palette_n * 3);
   }
   if (s->trns_len)
      png_chunk(&png, "tRNS", s->trns, s->trns_len);

   zlib_store(&compressed, filtered.data, filtered.len);
   at = 0;
   do {
      int n = compressed.len - at;
      if (s->idat_chunk_size > 0 && n > s->idat_chunk_size)
         n = s->idat_chunk_size;
      png_chunk(&png, "IDAT", compressed.data + at, n);
      at += n;
   } while (at < compressed.len);
   png_chunk(&png, "IEND", NULL, 0);

   free(filtered.data);
   free(compressed.data);
   *out_len = png.len;
   return png.data;
}

typedef struct
{
   int x, y, w, h;
   unsigned char *rows;
   int row_count;
   int abort_after;
} capture;

static int capture_row(void *user, int y, const unsigned char *rgba)
{
   capture *c = (capture *) user;
   if (c->abort_after >= 0 && c->row_count >= c->abort_after)
      return 0;
   if (y != c->y + c->row_count)
      return 0;
   memcpy(c->rows + (size_t) c->row_count * c->w * 4, rgba, (size_t) c->w * 4);
   ++c->row_count;
   return 1;
}

static int run_region(const unsigned char *png, int png_len,
                      int x, int y, int w, int h, int chunk_size,
                      unsigned char *rows, int *row_count, int abort_after,
                      size_t *peak_extra)
{
   stbi_region_decoder *decoder;
   capture cap;
   int at = 0;
   size_t before = rg_live_bytes;

   cap.x = x;
   cap.y = y;
   cap.w = w;
   cap.h = h;
   cap.rows = rows;
   cap.row_count = 0;
   cap.abort_after = abort_after;

   rg_peak_bytes = rg_live_bytes;
   rg_track_allocations = 1;
   decoder = stbi_region_begin(x, y, w, h, capture_row, &cap);
   if (!decoder) {
      rg_track_allocations = 0;
      return 0;
   }
   while (at < png_len) {
      int n = png_len - at;
      if (chunk_size > 0 && n > chunk_size)
         n = chunk_size;
      if (!stbi_region_feed(decoder, png + at, n)) {
         stbi_region_free(decoder);
         rg_track_allocations = 0;
         return 0;
      }
      at += n;
   }
   if (!stbi_region_finish(decoder)) {
      stbi_region_free(decoder);
      rg_track_allocations = 0;
      return 0;
   }
   *row_count = cap.row_count;
   if (peak_extra)
      *peak_extra = rg_peak_bytes - before;
   stbi_region_free(decoder);
   rg_track_allocations = 0;
   return 1;
}

static unsigned long long fnv1a(unsigned long long h, const unsigned char *data, size_t len)
{
   size_t i;
   for (i=0; i < len; ++i) {
      h ^= data[i];
      h *= 1099511628211ull;
   }
   return h;
}

typedef struct
{
   const char *name;
   int regions;
   long long pixels;
   int mismatches;
   int failures;
   size_t peak_extra;
   unsigned long long digest;
} verify_stats;

static void add_rect(int *n, int rects[][4], int x, int y, int w, int h)
{
   rects[*n][0] = x;
   rects[*n][1] = y;
   rects[*n][2] = w;
   rects[*n][3] = h;
   ++*n;
}

static int verify_png(const char *name, const unsigned char *png, int png_len,
                      const int chunk_sizes[], int chunk_count,
                      verify_stats *stats)
{
   unsigned char *full;
   unsigned char *region;
   int w = 0, h = 0, comp = 0;
   int rects[16][4];
   int rect_count = 0;
   int ri, ci;
   int failed = 0;

   full = stbi_load_from_memory(png, png_len, &w, &h, &comp, 4);
   if (!full) {
      printf("%-20s full decode failed: %s\n", name, stbi_failure_reason());
      return 0;
   }

   add_rect(&rect_count, rects, 0, 0, w, h);
   add_rect(&rect_count, rects, 0, 0, 1, 1);
   add_rect(&rect_count, rects, w-1, 0, 1, 1);
   add_rect(&rect_count, rects, 0, h-1, 1, 1);
   add_rect(&rect_count, rects, w-1, h-1, 1, 1);
   add_rect(&rect_count, rects, 0, 0, 1, h);
   add_rect(&rect_count, rects, w-1, 0, 1, h);
   add_rect(&rect_count, rects, 0, 0, w, 1);
   add_rect(&rect_count, rects, 0, h-1, w, 1);
   if (w > 2 && h > 2)
      add_rect(&rect_count, rects, 1, 1, w-2, h-2);
   if (w > 4 && h > 3)
      add_rect(&rect_count, rects, 2, 1, w-3, 2);

   stats->name = name;
   stats->digest = 1469598103934665603ull;
   for (ri=0; ri < rect_count; ++ri) {
      int x = rects[ri][0], y = rects[ri][1], rw = rects[ri][2], rh = rects[ri][3];
      region = (unsigned char *) malloc((size_t) rw * rh * 4);
      if (!region)
         exit(2);
      for (ci=0; ci < chunk_count; ++ci) {
         int rows = 0;
         size_t peak = 0;
         int row;
         if (!run_region(png, png_len, x, y, rw, rh, chunk_sizes[ci],
                         region, &rows, -1, &peak)) {
            printf("%-20s region %d,%d %dx%d chunk=%d failed\n",
                   name, x, y, rw, rh, chunk_sizes[ci]);
            ++stats->failures;
            failed = 1;
            continue;
         }
         if (rows != rh)
            ++stats->failures;
         if (peak > stats->peak_extra)
            stats->peak_extra = peak;
         ++stats->regions;
         stats->pixels += (long long) rw * rh;
         for (row=0; row < rh; ++row) {
            const unsigned char *ref = full + ((size_t) (y+row) * w + x) * 4;
            const unsigned char *got = region + (size_t) row * rw * 4;
            stats->digest = fnv1a(stats->digest, ref, (size_t) rw * 4);
            if (memcmp(ref, got, (size_t) rw * 4) != 0) {
               ++stats->mismatches;
               failed = 1;
            }
         }
      }
      free(region);
   }
   stbi_image_free(full);
   return !failed;
}

static int read_file(const char *path, unsigned char **data, int *len)
{
   FILE *f = fopen(path, "rb");
   long size;
   if (!f)
      return 0;
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   rewind(f);
   if (size <= 0 || size > 0x7fffffffL) {
      fclose(f);
      return 0;
   }
   *data = (unsigned char *) malloc((size_t) size);
   if (!*data || fread(*data, 1, (size_t) size, f) != (size_t) size) {
      fclose(f);
      free(*data);
      return 0;
   }
   fclose(f);
   *len = (int) size;
   return 1;
}

int main(void)
{
   static const int chunks[] = { 1, 2, 3, 7, 31, 257, 4096, 0 };
   verify_stats totals = { "TOTAL", 0, 0, 0, 0, 0, 1469598103934665603ull };
   int failures = 0;
   int i;

   crc_init();

   {
      static const struct {
         const char *name;
         int w, h, depth, color, palette_n;
      } cases[] = {
         { "rgba8_all_filters", 13, 9, 8, 6, 0 },
         { "rgb8_all_filters", 11, 7, 8, 2, 0 },
         { "ga8_all_filters", 9, 6, 8, 4, 0 },
         { "gray8_trns", 10, 7, 8, 0, 0 },
         { "gray1", 17, 5, 1, 0, 0 },
         { "gray2", 9, 4, 2, 0, 0 },
         { "gray4", 9, 4, 4, 0, 0 },
         { "palette4_trns", 17, 7, 4, 3, 16 },
         { "palette8", 13, 6, 8, 3, 32 },
         { "gray16_trns", 8, 6, 16, 0, 0 },
         { "rgb16_trns", 7, 5, 16, 2, 0 },
         { "rgba16", 6, 5, 16, 6, 0 }
      };
      for (i=0; i < (int) (sizeof(cases) / sizeof(cases[0])); ++i) {
         png_spec spec;
         unsigned char *png;
         int png_len;
         verify_stats stats;
         memset(&spec, 0, sizeof(spec));
         memset(&stats, 0, sizeof(stats));
         spec.width = cases[i].w;
         spec.height = cases[i].h;
         spec.depth = cases[i].depth;
         spec.color = cases[i].color;
         spec.palette_n = cases[i].palette_n;
         spec.idat_chunk_size = (i & 1) ? 13 : 0;
         spec.filter_seed = i;
         spec.trns_len = 0;
         if (spec.color == 3 && i == 7) {
            int j;
            spec.trns_len = 5;
            for (j=0; j < 5; ++j)
               spec.trns[j] = (unsigned char) (255 - j * 23);
         } else if (spec.color == 0 && spec.depth <= 8 && i == 3) {
            unsigned key = sample_value(0, 0, 0, spec.depth);
            spec.trns_len = 2;
            spec.trns[0] = 0;
            spec.trns[1] = (unsigned char) key;
         } else if (spec.color == 0 && spec.depth == 16 && i == 9) {
            unsigned key = sample_value(0, 0, 0, 16);
            spec.trns_len = 2;
            spec.trns[0] = (unsigned char) (key >> 8);
            spec.trns[1] = (unsigned char) key;
         } else if (spec.color == 2 && spec.depth == 16 && i == 10) {
            unsigned k0 = sample_value(0, 0, 0, 16);
            unsigned k1 = sample_value(0, 0, 1, 16);
            unsigned k2 = sample_value(0, 0, 2, 16);
            spec.trns_len = 6;
            spec.trns[0] = (unsigned char) (k0 >> 8);
            spec.trns[1] = (unsigned char) k0;
            spec.trns[2] = (unsigned char) (k1 >> 8);
            spec.trns[3] = (unsigned char) k1;
            spec.trns[4] = (unsigned char) (k2 >> 8);
            spec.trns[5] = (unsigned char) k2;
         }
         png = build_png(&spec, &png_len);
         if (!verify_png(cases[i].name, png, png_len, chunks,
                         (int) (sizeof(chunks)/sizeof(chunks[0])), &stats)) {
            ++failures;
         }
         printf("%-20s regions=%3d pixels=%7lld mismatches=%d peak_region=%6lu\n",
                stats.name, stats.regions, stats.pixels, stats.mismatches,
                (unsigned long) stats.peak_extra);
         totals.regions += stats.regions;
         totals.pixels += stats.pixels;
         totals.mismatches += stats.mismatches;
         totals.failures += stats.failures;
         if (stats.peak_extra > totals.peak_extra)
            totals.peak_extra = stats.peak_extra;
         totals.digest = fnv1a(totals.digest, (const unsigned char *) &stats.digest,
                               sizeof(stats.digest));
         free(png);
      }
   }

   {
      static const int file_chunks[] = { 1, 7, 4096, 0 };
      static const char *files[] = {
         "../data/map_01.png",
         "../data/atari_8bit_font_revised.png",
         "../data/easy_font_raw.png"
      };
      for (i=0; i < (int) (sizeof(files) / sizeof(files[0])); ++i) {
         unsigned char *png;
         int png_len;
         verify_stats stats;
         if (!read_file(files[i], &png, &png_len)) {
            printf("%-20s skipped (file not found)\n", files[i]);
            continue;
         }
         memset(&stats, 0, sizeof(stats));
         if (!verify_png(files[i], png, png_len, file_chunks,
                         (int) (sizeof(file_chunks)/sizeof(file_chunks[0])), &stats))
            ++failures;
         printf("%-20s regions=%3d pixels=%7lld mismatches=%d peak_region=%6lu\n",
                files[i], stats.regions, stats.pixels, stats.mismatches,
                (unsigned long) stats.peak_extra);
         totals.regions += stats.regions;
         totals.pixels += stats.pixels;
         totals.mismatches += stats.mismatches;
         totals.failures += stats.failures;
         if (stats.peak_extra > totals.peak_extra)
            totals.peak_extra = stats.peak_extra;
         totals.digest = fnv1a(totals.digest, (const unsigned char *) &stats.digest,
                               sizeof(stats.digest));
         free(png);
      }
   }

   {
      enum { W = 37, H = 19 };
      unsigned char pixels[W*H*4];
      unsigned char *png;
      int png_len;
      int x, y;
      verify_stats stats;
      static const int write_chunks[] = { 1, 5, 64, 0 };
      for (y=0; y < H; ++y) {
         for (x=0; x < W; ++x) {
            unsigned char *p = pixels + (y*W + x)*4;
            p[0] = (unsigned char) (x * 7 + y * 3);
            p[1] = (unsigned char) (x * 11 + y * 5);
            p[2] = (unsigned char) (x * 13 + y * 17);
            p[3] = (unsigned char) (x * 19 + y * 23);
         }
      }
      png = stbi_write_png_to_mem(pixels, W*4, W, H, 4, &png_len);
      if (!png) {
         printf("could not build fixed-Huffman test PNG\n");
         ++failures;
      } else {
         memset(&stats, 0, sizeof(stats));
         if (!verify_png("fixed_huffman_rgba8", png, png_len, write_chunks,
                         (int) (sizeof(write_chunks)/sizeof(write_chunks[0])), &stats))
            ++failures;
         printf("%-20s regions=%3d pixels=%7lld mismatches=%d peak_region=%6lu\n",
                "fixed_huffman_rgba8", stats.regions, stats.pixels,
                stats.mismatches, (unsigned long) stats.peak_extra);
         totals.regions += stats.regions;
         totals.pixels += stats.pixels;
         totals.mismatches += stats.mismatches;
         totals.failures += stats.failures;
         if (stats.peak_extra > totals.peak_extra)
            totals.peak_extra = stats.peak_extra;
         totals.digest = fnv1a(totals.digest, (const unsigned char *) &stats.digest,
                               sizeof(stats.digest));
         free(png);
      }
   }

   {
      png_spec spec;
      unsigned char *png;
      int png_len;
      unsigned char *rows = (unsigned char *) malloc(64 * 4);
      stbi_region_decoder *d;

      memset(&spec, 0, sizeof(spec));
      spec.width = 8;
      spec.height = 8;
      spec.depth = 8;
      spec.color = 6;
      png = build_png(&spec, &png_len);

      if (stbi_region_begin(-1, 0, 1, 1, capture_row, NULL) != NULL ||
          stbi_region_begin(0, 0, 0, 1, capture_row, NULL) != NULL ||
          stbi_region_begin(0, 0, 1, 1, NULL, NULL) != NULL) {
         printf("invalid begin arguments were accepted\n");
         ++failures;
      }

      d = stbi_region_begin(7, 7, 2, 1, capture_row, NULL);
      if (d) {
         if (stbi_region_feed(d, png, png_len) != 0 ||
             stbi_region_complete(d)) {
            printf("out-of-bounds region was accepted\n");
            ++failures;
         }
         stbi_region_free(d);
      } else {
         printf("out-of-bounds begin unexpectedly returned NULL\n");
         ++failures;
      }

      {
         capture cap;
         cap.x = 0; cap.y = 0; cap.w = 8; cap.h = 8;
         cap.rows = rows; cap.row_count = 0; cap.abort_after = -1;
         d = stbi_region_begin(0, 0, 8, 8, capture_row, &cap);
         if (!d)
            exit(2);
         if (stbi_region_feed(d, png, png_len / 2) != 1 ||
             stbi_region_finish(d) != 0 || stbi_region_complete(d)) {
            printf("truncated stream was not rejected\n");
            ++failures;
         }
         stbi_region_free(d);
      }

      {
         capture cap;
         cap.x = 0; cap.y = 0; cap.w = 8; cap.h = 8;
         cap.rows = rows; cap.row_count = 0; cap.abort_after = 3;
         d = stbi_region_begin(0, 0, 8, 8, capture_row, &cap);
         if (!d)
            exit(2);
         if (stbi_region_feed(d, png, png_len) != 0 ||
             stbi_region_complete(d) ||
             strstr(stbi_region_failure_reason(d), "callback") == NULL) {
            printf("callback abort was not reported\n");
            ++failures;
         }
         stbi_region_free(d);
      }

      d = stbi_region_begin(0, 0, 8, 8, capture_row, NULL);
      if (!d)
         exit(2);
      if (stbi_region_feed(d, NULL, 1) != 0 || stbi_region_feed(d, png, -1) != 0) {
         printf("invalid feed arguments were accepted\n");
         ++failures;
      }
      stbi_region_free(d);

      spec.interlace = 1;
      free(png);
      png = build_png(&spec, &png_len);
      d = stbi_region_begin(0, 0, 8, 8, capture_row, NULL);
      if (!d)
         exit(2);
      if (stbi_region_feed(d, png, png_len) != 0 ||
          strstr(stbi_region_failure_reason(d), "interlaced") == NULL) {
         printf("interlaced PNG was not rejected explicitly\n");
         ++failures;
      }
      stbi_region_free(d);
      free(png);
      free(rows);
   }

   printf("\nSUMMARY regions=%d pixels=%lld mismatches=%d failures=%d peak_region=%lu digest=%016llx\n",
          totals.regions, totals.pixels, totals.mismatches, totals.failures,
          (unsigned long) totals.peak_extra, totals.digest);
   if (totals.mismatches || totals.failures || failures)
      return 1;
   return 0;
}
