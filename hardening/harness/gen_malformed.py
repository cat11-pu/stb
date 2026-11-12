#!/usr/bin/env python3
"""Generate a reproducible 2000-file malformed-image corpus.

Every file derives from a known-valid seed (hardening/valid + pngsuite) or a
hand-crafted structure, then gets mutated in ways that historically crash
naive decoders: truncation, header/ dimension tampering, byte flips,
insertion/deletion, zlib/stream corruption, and oversized-dimension bombs.
A fixed RNG seed makes the corpus deterministic.
"""
import os
import struct
import random
import glob

RNG = random.Random(0x57B17A6E)  # "STBIANE"
OUT = os.path.join(os.path.dirname(__file__), "..", "corpus")
SEED_DIR = os.path.join(os.path.dirname(__file__), "..", "valid")


def write(name, data):
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(data)


def truncations(name, data, bucket):
    # truncate at structurally interesting + random points
    points = {1, 2, 4, 8, 16, 32, 64, 128, 256}
    for k in range(10):
        points.add(RNG.randrange(1, max(2, len(data))))
    for i, cut in enumerate(sorted(p for p in points if p < len(data))):
        write(f"{bucket}_{name}_trunc{i:02d}_{cut}.bin", data[:cut])


def flips(name, data, bucket, count=10):
    for i in range(count):
        buf = bytearray(data)
        for _ in range(RNG.randrange(1, 5)):
            pos = RNG.randrange(len(buf))
            buf[pos] ^= 1 << RNG.randrange(8)
        write(f"{bucket}_{name}_flip{i:02d}.bin", bytes(buf))


def multi_flips(name, data, bucket, count=8):
    # heavy corruption: many scattered bit flips (still starts from a real header)
    for i in range(count):
        buf = bytearray(data)
        for _ in range(RNG.randrange(8, 40)):
            pos = RNG.randrange(len(buf))
            buf[pos] ^= 1 << RNG.randrange(8)
        write(f"{bucket}_{name}_multiflip{i:02d}.bin", bytes(buf))


def tamper(name, data, bucket, count=10):
    # overwrite runs with hostile values (zeros, 0xff, large lengths)
    for i in range(count):
        buf = bytearray(data)
        pos = RNG.randrange(len(buf))
        run = min(RNG.randrange(1, 9), len(buf) - pos)
        val = RNG.choice([0, 0xFF, 0x7F, 0x80])
        buf[pos:pos + run] = bytes([val]) * run
        write(f"{bucket}_{name}_tamper{i:02d}.bin", bytes(buf))


def inserts_deletes(name, data, bucket, count=9):
    for i in range(count):
        buf = bytearray(data)
        if RNG.random() < 0.5 and len(buf) > 4:
            pos = RNG.randrange(len(buf))
            del buf[pos:pos + RNG.randrange(1, min(8, len(buf) - pos) + 1)]
        else:
            pos = RNG.randrange(len(buf) + 1)
            buf[pos:pos] = bytes(RNG.randrange(256) for _ in range(RNG.randrange(1, 9)))
        write(f"{bucket}_{name}_insdel{i:02d}.bin", bytes(buf))


def header_smashes(name, data, bucket, count=8):
    # overwrite the first 32 bytes specifically, where all dimension/type
    # headers live, while keeping a plausible magic where possible
    for i in range(count):
        buf = bytearray(data)
        limit = min(32, len(buf))
        for _ in range(RNG.randrange(1, limit + 1)):
            pos = RNG.randrange(limit)
            buf[pos] = RNG.choice([0, 0xFF, 0x7F, 0x80, RNG.randrange(256)])
        write(f"{bucket}_{name}_hdrsmash{i:02d}.bin", bytes(buf))


def dimension_bombs(name, data, bucket):
    """Inject absurd width/height fields where the format exposes them."""
    produced = []
    if name.endswith(".bmp"):
        # BMP DIB header: width at +18, height at +22 (little-endian int32)
        for off, field in ((18, "w"), (22, "h")):
            for val in (0x7FFFFFFF, 0x40000000, 0x00010000):
                buf = bytearray(data)
                buf[off:off + 4] = struct.pack("<I", val)
                produced.append((f"{bucket}_{name}_bomb_{field}_{val:x}.bin", bytes(buf)))
    elif name.endswith(".png"):
        # IHDR: width at byte 16..19, height 20..23 (big-endian)
        for off, field in ((16, "w"), (20, "h")):
            for val in (0x7FFFFFFF, 0x40000000, 0x00010000):
                buf = bytearray(data)
                buf[off:off + 4] = struct.pack(">I", val)
                produced.append((f"{bucket}_{name}_bomb_{field}_{val:x}.bin", bytes(buf)))
    elif name.endswith(".gif"):
        # logical screen width/height at 6..9 (little-endian u16)
        for off, field in ((6, "w"), (8, "h")):
            for val in (0xFFFF, 0x8000):
                buf = bytearray(data)
                buf[off:off + 2] = struct.pack("<H", val)
                produced.append((f"{bucket}_{name}_bomb_{field}_{val:x}.bin", bytes(buf)))
    elif name in ("rgb8.ppm", "gray8.pgm", "gray16.pgm"):
        # rewrite the "W H" header token with huge dimensions
        head_end = data.index(b"\n", data.index(b"\n") + 1) + 1
        head_end = data.index(b"\n", head_end) + 1  # skip maxval line too
        for val in (2147483647, 1 << 20):
            bomb = (data[:data.index(b" ", 3)].split(b"\n")[0] + b"\n" +
                    f"{val} {val}\n255\n".encode())
            produced.append((f"{bucket}_{name}_bomb_{val:x}.bin", bomb))
    elif name.endswith(".tga"):
        # TGA image dimensions at header offset 12 (w) and 14 (h), LE u16
        for off, field in ((12, "w"), (14, "h")):
            for val in (0xFFFF, 0x8000):
                buf = bytearray(data)
                buf[off:off + 2] = struct.pack("<H", val)
                produced.append((f"{bucket}_{name}_bomb_{field}_{val:x}.bin", bytes(buf)))
    elif name.endswith(".psd"):
        for off, field in ((14, "h"), (18, "w")):
            for val in (0x7FFFFFFF, 0x00010000):
                buf = bytearray(data)
                buf[off:off + 4] = struct.pack(">I", val)
                produced.append((f"{bucket}_{name}_bomb_{field}_{val:x}.bin", bytes(buf)))
    for n, d in produced:
        write(n, d)


def handmade():
    """Structurally hostile inputs built from scratch (not seed mutations)."""
    items = []
    # empty / near-empty
    items.append(("hm_empty0.bin", b""))
    items.append(("hm_empty1.bin", b"\x00"))
    items.append(("hm_rand16.bin", bytes(RNG.randrange(256) for _ in range(16))))
    items.append(("hm_rand256.bin", bytes(RNG.randrange(256) for _ in range(256))))
    # magic only, every supported format
    magics = {
        "png": b"\x89PNG\r\n\x1a\n",
        "jpg": b"\xff\xd8\xff\xe0" + b"\x00\x10JFIF\x00",
        "gif": b"GIF89a",
        "bmp": b"BM",
        "psd": b"8BPS\x00\x01",
        "hdr": b"#?RADIANCE\n",
        "ppm": b"P6\n64 64\n255\n",
        "pgm": b"P5\n64 64\n255\n",
        "tga": b"\x00\x00\x00\x00",
    }
    for k, (fmt, magic) in enumerate(magics.items()):
        items.append((f"hm_magic_{fmt}.bin", magic))
        items.append((f"hm_magic_{fmt}_zeros.bin", magic + b"\x00" * 200))
        items.append((f"hm_magic_{fmt}_ff.bin", magic + b"\xff" * 200))
    # PNG with declared IHDR length larger than file
    ihdr = b"\x00\x00\x00\xffIHDR" + b"\x00" * 20
    items.append(("hm_png_bad_ihdr_len.bin", magics["png"] + ihdr))
    # PNG with zero dimensions
    items.append(("hm_png_zero_dims.bin",
                  magics["png"] + b"\x00\x00\x00\x0dIHDR" +
                  b"\x00" * 8 + b"\x08\x02\x00\x00\x00"))
    # PNG whose decoded output (~400 MB) exceeds the 256 MB allocation cap,
    # while staying under STBI_MAX_DIMENSIONS (16 MP per side)
    def png_chunk(typ, data):
        import zlib as _z
        body = typ + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", _z.crc32(body) & 0xffffffff))
    items.append(("hm_png_alloc_bomb.bin",
                  magics["png"] +
                  png_chunk(b"IHDR", struct.pack(">IIBBBBB", 20000, 20000, 8, 0, 0, 0, 0)) +
                  png_chunk(b"IDAT", __import__("zlib").compress(b"\x00" * 100)) +
                  png_chunk(b"IEND", b"")))
    # zlib decompression bomb: 314 KB file expands past 256 MB during
    # zlib output growth, exercising the realloc cap path
    import zlib as _z2
    bw = bh = 9000
    co = _z2.compressobj(9)
    zparts = [co.compress(b"\x00" + b"\x00" * bw * 4) for _ in range(bh)]
    zparts.append(co.flush())
    items.append(("hm_png_zlib_bomb.bin",
                  magics["png"] +
                  png_chunk(b"IHDR", struct.pack(">IIBBBBB", bw, bh, 8, 6, 0, 0, 0)) +
                  png_chunk(b"IDAT", b"".join(zparts)) +
                  png_chunk(b"IEND", b"")))
    # JPEG with SOF claiming huge dimensions but no scan data
    sof = b"\xff\xc0\x00\x11\x08" + struct.pack(">HH", 0xffff, 0xffff) + b"\x03\x01\x11\x00\x02\x11\x00\x03\x11\x00"
    items.append(("hm_jpg_huge_sof.bin", b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00" + sof))
    # GIF terminator immediately after header
    items.append(("hm_gif_terminator.bin", b"GIF89a\x10\x00\x10\x00\x00\x00\x00\x3b"))
    # GIF image descriptor with 0x00 size
    items.append(("hm_gif_zero_descr.bin",
                  b"GIF89a\x01\x00\x01\x00\x80\x00\x00" + b"\x00" * 768 +
                  b"\x2c\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x02\x00\x3b"))
    # nested/recursive-looking zlib streams and corrupted zlib headers
    items.append(("hm_zlib_bad.bin", b"\x78\x9c" + bytes(RNG.randrange(256) for _ in range(300))))
    items.append(("hm_zlib_trunc.bin", b"\x78\x9c\x01\x02\x00\xef\xff"))
    # BMP with offset-to-pixels beyond file
    bmp = bytearray(b"BM" + struct.pack("<I", 4096) + b"\x00\x00\x00\x00" +
                    struct.pack("<I", 54) + b"\x28\x00\x00\x00" +
                    struct.pack("<ii", 64, 64) + b"\x01\x00\x08\x00" + b"\x00" * 28)
    items.append(("hm_bmp_bad_offset.bin", bytes(bmp)))
    # HDR with malformed resolution line
    items.append(("hm_hdr_bad_res.bin", b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y notanumber +X 32\n"))
    # PSD header then EOF
    items.append(("hm_psd_header_only.bin",
                  b"8BPS\x00\x01" + b"\x00" * 6 + struct.pack(">HIIHH", 3, 64, 64, 8, 3)))
    # PNM with absurd tokens
    items.append(("hm_pnm_huge.bin", b"P6\n999999999 999999999\n255\n"))
    items.append(("hm_pnm_negative.bin", b"P6\n-1 -1\n255\n"))
    items.append(("hm_pnm_garbage.bin", b"P6\nabc def\n255\n" + b"\xff" * 50))
    # lengths that exploit sign extension / int32 boundaries
    for val in (0x7FFFFFFF, 0x80000000, 0xFFFFFFFF):
        items.append((f"hm_len_{val:x}.bin",
                      magics["bmp"] + struct.pack("<I", val) * 8))
    for name, data in items:
        write(name, data)
    return len(items)


def main():
    os.makedirs(OUT, exist_ok=True)
    for old in glob.glob(os.path.join(OUT, "*.bin")):
        os.remove(old)
    seeds = sorted(glob.glob(os.path.join(SEED_DIR, "*")))
    seeds += sorted(glob.glob(os.path.join(os.path.dirname(__file__), "..", "..",
                                           "tests", "pngsuite", "primary", "*.png")))[:12]
    total = 0
    for path in seeds:
        name = os.path.basename(path)
        data = open(path, "rb").read()
        bucket = name.rsplit(".", 1)[-1].lower()
        before = len(glob.glob(os.path.join(OUT, "*.bin")))
        truncations(name, data, bucket)
        flips(name, data, bucket)
        multi_flips(name, data, bucket)
        tamper(name, data, bucket)
        inserts_deletes(name, data, bucket)
        header_smashes(name, data, bucket)
        dimension_bombs(name, data, bucket)
        total += len(glob.glob(os.path.join(OUT, "*.bin"))) - before
    total += handmade()
    n = len(glob.glob(os.path.join(OUT, "*.bin")))
    TARGET = 2000
    files = sorted(glob.glob(os.path.join(OUT, "*.bin")))
    if len(files) > TARGET:
        # deterministic stratified trim so every seed/format family survives
        keep = set()
        step = len(files) / TARGET
        for i in range(TARGET):
            keep.add(files[int(i * step)])
        for path in files:
            if path not in keep:
                os.remove(path)
    n = len(glob.glob(os.path.join(OUT, "*.bin")))
    print(f"generated {n} malformed samples in {OUT}")


if __name__ == "__main__":
    main()
