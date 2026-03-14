#!/usr/bin/env python3
"""
local_parallel_search.py  —  Local parallel search (runs immediately)

Equation:
    Y^2 = (m + 6n)^2 + (36n^3 - 19) / m     (m ≠ 0, m | 36n^3-19)

Searches ALL integers n (positive and negative) in an expanding spiral:
    0, 1, -1, 2, -2, 3, -3, ...

Uses multiprocessing for speed. Runs indefinitely until Ctrl+C.

Usage:
    python local_parallel_search.py [--workers N] [--band B] [--start_n N]
"""

import sys
import os
import math
import time
import argparse
import multiprocessing as mp
import json
import signal
from pathlib import Path

CHECKPOINT_FILE = "checkpoint_local.json"
SOLUTIONS_FILE  = "solutions_local.txt"
LOG_FILE        = "local_search.log"

# ─────────────────────────────────────────────────────────────────────────────
def isqrt(n: int) -> int:
    return math.isqrt(n)

def is_perfect_square(n: int):
    if n < 0:
        return False, None
    r = math.isqrt(n)
    return (r * r == n), r

def all_divisors(val: int):
    if val == 0:
        return
    absval = abs(val)
    d = 1
    while d * d <= absval:
        if absval % d == 0:
            yield d
            yield -d
            q = absval // d
            if q != d:
                yield q
                yield -q
        d += 1

# ─────────────────────────────────────────────────────────────────────────────
def search_band(args):
    """
    Worker function for multiprocessing.Pool.
    args: (n_start, n_end, worker_id)
    Returns list of solution dicts.
    """
    n_start, n_end, wid = args
    found = []
    for n in range(n_start, n_end + 1):
        val = 36 * n * n * n - 19
        if val == 0:
            continue
        for m in all_divisors(val):
            quot = val // m
            X    = m + 6 * n
            rhs  = X * X + quot
            ok, Y = is_perfect_square(rhs)
            if ok:
                found.append({"n": n, "m": m, "Y": Y, "X": X})
                if Y != 0:
                    found.append({"n": n, "m": m, "Y": -Y, "X": X})
    return found

# ─────────────────────────────────────────────────────────────────────────────
def save_checkpoint(state: dict):
    with open(CHECKPOINT_FILE, "w") as f:
        json.dump(state, f)

def load_checkpoint() -> dict:
    if Path(CHECKPOINT_FILE).exists():
        with open(CHECKPOINT_FILE) as f:
            return json.load(f)
    return {"max_searched": 0, "total_n_searched": 0}

def append_solutions(solutions: list):
    if not solutions:
        return
    with open(SOLUTIONS_FILE, "a") as f:
        for s in solutions:
            f.write(f"n={s['n']} m={s['m']} Y={s['Y']} X={s['X']}\n")

def log(msg: str):
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line, flush=True)
    with open(LOG_FILE, "a") as f:
        f.write(line + "\n")

# ─────────────────────────────────────────────────────────────────────────────
def n_spiral(start: int = 0):
    """
    Yield integers in the spiral: start, start+1, start-1, start+2, start-2, ...
    Actually we keep the sign: 0, 1, -1, 2, -2, ...
    More precisely we yield n = 0, 1, -1, 2, -2, 3, -3, ...
    If start > 0, we resume from start.
    """
    k = start
    while True:
        yield k
        yield -(k + 1)
        k += 1

def batched_n_stream(band: int, start_abs: int = 0):
    """
    Yields (pos_start, pos_end, neg_start, neg_end) pairs where:
    - pos_start..pos_end is a band of positive n values
    - neg_start..neg_end is a band of negative n values
    Each covers `band` values.
    """
    pos = start_abs
    neg = -(start_abs + 1)
    while True:
        yield (pos, pos + band - 1, neg - band + 1, neg)
        pos += band
        neg -= band

# ─────────────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Local parallel search for Y^2=(m+6n)^2+(36n^3-19)/m")
    parser.add_argument("--workers", type=int,
                        default=max(1, mp.cpu_count() - 1),
                        help="Number of parallel workers")
    parser.add_argument("--band", type=int, default=500,
                        help="n-range per work chunk")
    parser.add_argument("--start_n", type=int, default=None,
                        help="Override checkpoint: start from this |n|")
    parser.add_argument("--no_limit", action="store_true",
                        help="Run indefinitely (default: yes)")
    args = parser.parse_args()

    log("=" * 60)
    log("Equation: Y^2 = (m+6n)^2 + (36n^3-19)/m")
    log(f"Workers: {args.workers}  Band: {args.band}")

    ckpt = load_checkpoint()
    start_abs = args.start_n if args.start_n is not None \
                else ckpt.get("max_searched", 0)
    total_searched = ckpt.get("total_n_searched", 0)
    total_solutions = ckpt.get("total_solutions", 0)
    log(f"Resuming from |n| = {start_abs}  (total searched so far: {total_searched})")

    stop_flag = mp.Event()

    def handler(sig, frame):
        log("Interrupted — saving checkpoint …")
        stop_flag.set()
    signal.signal(signal.SIGINT,  handler)
    signal.signal(signal.SIGTERM, handler)

    pool  = mp.Pool(processes=args.workers)
    band  = args.band
    pos   = start_abs
    t0    = time.time()

    for pos_s, pos_e, neg_s, neg_e in batched_n_stream(band, start_abs):
        if stop_flag.is_set():
            break

        # Submit both positive and negative bands as separate work items
        work_items = [
            (pos_s, pos_e, 0),
            (neg_s, neg_e, 1),
        ]

        results = pool.map(search_band, work_items)
        all_found = results[0] + results[1]
        append_solutions(all_found)

        total_searched += 2 * band
        total_solutions += len(all_found)
        pos = pos_e + 1

        elapsed  = time.time() - t0
        rate     = total_searched / max(elapsed, 1e-9)

        ckpt_state = {
            "max_searched":    pos,
            "total_n_searched": total_searched,
            "total_solutions": total_solutions,
        }
        save_checkpoint(ckpt_state)

        if all_found:
            for s in all_found:
                log(f"★ SOLUTION: n={s['n']} m={s['m']} Y={s['Y']} X={s['X']}")

        log(f"|n| up to {pos_e}  |  searched {total_searched:,}  |  "
            f"solutions {total_solutions}  |  {rate:.0f} n/s  |  "
            f"elapsed {elapsed:.1f}s")

    pool.close()
    pool.join()
    log(f"Stopped. Total n searched: {total_searched:,}  Solutions: {total_solutions}")


if __name__ == "__main__":
    main()
