#!/usr/bin/env bash
# End-to-end hardening verification:
#   1. regenerate the 2000-sample malformed corpus (deterministic)
#   2. crash-test every sample under ASan+UBSan and a plain build
#   3. functional guard tests (allocation cap, error localization)
#   4. pixel-level decode consistency vs the pristine v2.30 baseline
#   5. performance regression check (median time, <=10% budget)
# All output goes to hardening/reports/.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
H="$ROOT/hardening"
R="$H/reports"
BASELINE_DIR=/tmp/stbi_hard_baseline
mkdir -p "$R" "$BASELINE_DIR"
cp "$H/baseline/stb_image_v2.30_orig.h" "$BASELINE_DIR/stb_image.h"

CC="${CC:-gcc}"
SAN_FLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
OPT_FLAGS="-O2"

echo "== [1/5] regenerate corpora =="
$CC $OPT_FLAGS "$H/harness/gen_valid.c" -lm -o "$R/gen_valid"
"$R/gen_valid"
python3 "$H/harness/gen_malformed.py"
N_CORPUS=$(find "$H/corpus" -name '*.bin' | wc -l)
echo "   malformed corpus: $N_CORPUS files"

echo "== [2/5] crash tests (ASan+UBSan and plain) =="
$CC $SAN_FLAGS "$H/harness/crash_driver.c" -I"$ROOT" -lm -o "$R/crash_asan"
$CC $OPT_FLAGS "$H/harness/crash_driver.c" -I"$ROOT" -lm -o "$R/crash_plain"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$R/crash_asan" "$H"/corpus/*.bin \
   2> "$R/crash_asan.err" | tee "$R/crash_asan.txt"
if [[ -s "$R/crash_asan.err" ]]; then echo "ASan reported errors"; cat "$R/crash_asan.err"; exit 1; fi
"$R/crash_plain" "$H"/corpus/*.bin 2> "$R/crash_plain.err" | tee "$R/crash_plain.txt"
if [[ -s "$R/crash_plain.err" ]]; then echo "plain driver warnings"; cat "$R/crash_plain.err"; exit 1; fi

echo "== [3/5] guard tests =="
$CC $SAN_FLAGS "$H/harness/test_guards.c" -I"$ROOT" -lm -o "$R/test_guards"
ASAN_OPTIONS=detect_leaks=0 "$R/test_guards" | tee "$R/guards.txt"

echo "== [4/5] pixel consistency vs v2.30 baseline =="
sed -e 's|../../stb_image.h|stb_image.h|' \
    -e 's|stbi_failure_offset()|(-1LL)|' \
    -e 's|stbi_failure_detail()|stbi_failure_reason()|' \
    "$H/harness/decode_dump.c" > "$BASELINE_DIR/decode_dump.c"
$CC $OPT_FLAGS "$BASELINE_DIR/decode_dump.c" -I"$BASELINE_DIR" -lm -o "$R/dump_baseline"
$CC $OPT_FLAGS "$H/harness/decode_dump.c" -I"$ROOT" -lm -o "$R/dump_hardened"
rm -rf "$R/pix_base" "$R/pix_hard"
mkdir -p "$R/pix_base" "$R/pix_hard"
# decode_dump returns 1 when inputs are rejected, which is expected here
"$R/dump_baseline" "$R/pix_base" "$H"/valid/* > "$R/manifest_base.txt" || true
"$R/dump_hardened" "$R/pix_hard" "$H"/valid/* > "$R/manifest_hard.txt" || true
DIFFS=0
for f in "$R/pix_base"/*.pix; do
   b=$(basename "$f")
   case "$b" in *hdr*) continue;; esac  # HDR intentionally fixed upstream (see README)
   cmp -s "$f" "$R/pix_hard/$b" || { echo "pixel diff: $b"; DIFFS=1; }
done
# pngsuite too
rm -rf "$R/png_base" "$R/png_hard"
mkdir -p "$R/png_base" "$R/png_hard"
"$R/dump_baseline" "$R/png_base" "$ROOT"/tests/pngsuite/*/*.png > "$R/png_manifest_base.txt" || true
"$R/dump_hardened" "$R/png_hard" "$ROOT"/tests/pngsuite/*/*.png > "$R/png_manifest_hard.txt" || true
for f in "$R/png_base"/*.pix; do
   b=$(basename "$f")
   cmp -s "$f" "$R/png_hard/$b" || { echo "png pixel diff: $b"; DIFFS=1; }
done
[[ $DIFFS -eq 0 ]] && echo "   all non-HDR pixel dumps byte-identical to v2.30"

echo "== [5/5] performance check =="
cat > "$R/genbig.c" <<'EOF'
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <stdlib.h>
static unsigned char* mk(int w,int h,int comp){
  unsigned char*d=malloc((size_t)w*h*comp);
  for(int y=0;y<h;y++)for(int x=0;x<w;x++)for(int c=0;c<comp;c++)
    d[((y*w+x)*comp)+c]=(unsigned char)((x*3+y*5+c*40+(x*y%19))&255);
  return d;
}
int main(void){
  int w=2048,h=2048;char p[128];
  unsigned char*d3=mk(w,h,3),*d4=mk(w,h,4),*d1=mk(w,h,1);
  stbi_write_png("/tmp/stbi_big.png",w,h,4,d4,w*4);
  stbi_write_jpg("/tmp/stbi_big.jpg",w,h,3,d3,92);
  stbi_write_bmp("/tmp/stbi_big.bmp",w,h,3,d3);
  stbi_write_tga("/tmp/stbi_big.tga",w,h,4,d4);
  stbi_write_png("/tmp/stbi_big_gray.png",w,h,1,d1,w);
  return 0;
}
EOF
$CC $OPT_FLAGS -I"$ROOT" "$R/genbig.c" -lm -o "$R/genbig"
"$R/genbig"
$CC $OPT_FLAGS -march=native "$H/harness/bench.c" -I"$ROOT" -lm -o "$R/bench_hard"
sed 's|../../stb_image.h|stb_image.h|' "$H/harness/bench.c" > "$BASELINE_DIR/bench.c"
$CC $OPT_FLAGS -march=native "$BASELINE_DIR/bench.c" -I"$BASELINE_DIR" -lm -o "$R/bench_base"
python3 "$H/harness/perf_compare.py" "$R/bench_base" "$R/bench_hard" \
   /tmp/stbi_big.png /tmp/stbi_big.jpg /tmp/stbi_big.bmp /tmp/stbi_big.tga /tmp/stbi_big_gray.png \
   | tee "$R/performance.txt"

echo
echo "ALL CHECKS COMPLETE - reports in $R"
