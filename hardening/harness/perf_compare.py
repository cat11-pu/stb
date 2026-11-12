#!/usr/bin/env python3
"""Alternating-run performance comparison with median aggregation.

Usage: perf_compare.py <base_bench> <hard_bench> <image> [image ...]
Exits non-zero if the hardened median is more than 10% slower on any image.
"""
import statistics
import subprocess
import sys

ROUNDS = 3
REPS = 40
BUDGET = 0.10


def run(bench, image):
    out = subprocess.run([bench, image, str(REPS)], capture_output=True, text=True, check=True)
    return float(out.stdout.split("\t")[1].strip().rstrip(" s"))


def main():
    base_bench, hard_bench, *images = sys.argv[1:]
    failed = False
    print(f"{'image':<28}{'base median (s)':>16}{'hard median (s)':>16}{'delta':>10}")
    for image in images:
        bt, ht = [], []
        for _ in range(ROUNDS):
            bt.append(run(base_bench, image))
            ht.append(run(hard_bench, image))
        bm, hm = statistics.median(bt), statistics.median(ht)
        delta = hm / bm - 1.0
        flag = " OK" if delta <= BUDGET else " OVER-BUDGET"
        print(f"{image.split('/')[-1]:<28}{bm:>16.4f}{hm:>16.4f}{delta*100:>9.2f}%{flag}")
        if delta > BUDGET:
            failed = True
    print("performance budget: +%.0f%% per-image decode time" % (BUDGET * 100))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
