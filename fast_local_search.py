#!/usr/bin/env python3
"""
fast_local_search.py  —  C-worker-backed local parallel search

Uses the compiled worker_s3ceq binary (100-1000x faster than pure Python)
to search all integers n for solutions to:

    Y^2 = (m + 6n)^2 + (36n^3 - 19) / m     (m ≠ 0, m | 36n^3-19)

Runs indefinitely, expanding outward from n=0 in both directions.
Uses multiprocessing to run multiple C workers in parallel.
All results are deduplicated and appended to solutions_master.txt.

Usage:
    python fast_local_search.py [--workers N] [--band B]
    python fast_local_search.py --workers 4 --band 10000
"""

import sys
import os
import re
import json
import time
import signal
import tempfile
import argparse
import subprocess
import multiprocessing as mp
from pathlib import Path

# ── paths ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR      = Path(__file__).parent.resolve()
WORKER_BIN      = SCRIPT_DIR / "worker_s3ceq"
CHECKPOINT_FILE = SCRIPT_DIR / "checkpoint_fast.json"
SOLUTIONS_FILE  = SCRIPT_DIR / "solutions_master.txt"
LOG_FILE        = SCRIPT_DIR / "fast_search.log"

SOLUTION_RE = re.compile(r"n=(-?\d+)\s+m=(-?\d+)\s+Y=(-?\d+)\s+X=(-?\d+)")

# ── adaptive band sizing ───────────────────────────────────────────────────────
def adaptive_band(n_abs: int, target_ops: float = 4e9) -> int:
    """
    Return band size so each task takes ~constant computational work.

    Work per n at |n|=N:  O(sqrt(|36n^3 - 19|)) ≈ 6 * N^(3/2)
    Total for a band of B values starting at N:  ≈ B * 6 * N^(3/2)
    Set equal to target_ops and solve for B.

    target_ops = 4e9 → ~4 seconds on a 1 GHz C worker per task (good for CE).
    Clamp to [1, 200_000].
    """
    if n_abs < 10:
        return 200_000   # tiny n: trivially fast, use huge bands
    work_per_n = 6.0 * (float(n_abs) ** 1.5)
    band = int(target_ops / work_per_n)
    return max(1, min(band, 200_000))

# ── logging ───────────────────────────────────────────────────────────────────
def log(msg: str):
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line, flush=True)
    with open(LOG_FILE, "a") as f:
        f.write(line + "\n")

# ── checkpoint ────────────────────────────────────────────────────────────────
def load_ckpt() -> dict:
    if CHECKPOINT_FILE.exists():
        with open(CHECKPOINT_FILE) as f:
            return json.load(f)
    return {"max_pos": 0, "max_neg": 0, "total_searched": 0, "total_solutions": 0}

def save_ckpt(ckpt: dict):
    with open(CHECKPOINT_FILE, "w") as f:
        json.dump(ckpt, f, indent=2)

# ── solution storage ──────────────────────────────────────────────────────────
def load_known_keys() -> set:
    known = set()
    if SOLUTIONS_FILE.exists():
        with open(SOLUTIONS_FILE) as f:
            for line in f:
                m = SOLUTION_RE.search(line)
                if m:
                    n, mv, Y = int(m.group(1)), int(m.group(2)), abs(int(m.group(3)))
                    known.add((n, mv, Y))
    return known

def save_solutions(solutions: list, known: set):
    new_count = 0
    with open(SOLUTIONS_FILE, "a") as f:
        for s in solutions:
            key = (s["n"], s["m"], abs(s["Y"]))
            if key not in known:
                known.add(key)
                f.write(f"n={s['n']} m={s['m']} Y={s['Y']} X={s['X']}\n")
                new_count += 1
    return new_count

# ── C worker subprocess ───────────────────────────────────────────────────────
def run_c_worker(n_start: int, n_end: int) -> list:
    """
    Run worker_s3ceq on [n_start, n_end] and return list of solution dicts.
    """
    if not WORKER_BIN.exists():
        raise FileNotFoundError(
            f"C worker not found: {WORKER_BIN}\n"
            f"Run 'make all' in {SCRIPT_DIR} first."
        )
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as wu:
        wu.write(f"{n_start} {n_end}\n")
        wu_path = wu.name
    out_path = wu_path + ".out"
    try:
        subprocess.run(
            [str(WORKER_BIN), wu_path, out_path],
            check=True, capture_output=True
        )
        solutions = []
        if Path(out_path).exists():
            with open(out_path) as f:
                for line in f:
                    m = SOLUTION_RE.search(line)
                    if m:
                        solutions.append({
                            "n": int(m.group(1)), "m": int(m.group(2)),
                            "Y": int(m.group(3)), "X": int(m.group(4))
                        })
        return solutions
    finally:
        Path(wu_path).unlink(missing_ok=True)
        Path(out_path).unlink(missing_ok=True)


def run_worker_task(args):
    """Wrapper for multiprocessing.Pool."""
    n_start, n_end = args
    return run_c_worker(n_start, n_end)


# ── main loop ─────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workers", type=int,
                        default=max(1, mp.cpu_count() - 1))
    parser.add_argument("--band", type=int, default=None,
                        help="Fixed band size (overrides adaptive sizing)")
    parser.add_argument("--target_ops", type=float, default=4e9,
                        help="Target ops per task for adaptive band (default 4e9 ≈ 4s)")
    parser.add_argument("--start_n", type=int, default=None)
    args = parser.parse_args()

    # Ensure C worker is built
    if not WORKER_BIN.exists():
        log(f"Building C worker…")
        subprocess.run(["make", "all"], cwd=SCRIPT_DIR, check=True)

    ckpt = load_ckpt()
    pos  = args.start_n if args.start_n is not None else ckpt["max_pos"]
    neg  = args.start_n if args.start_n is not None else ckpt["max_neg"]
    known = load_known_keys()

    log("=" * 60)
    log(f"Equation: Y^2 = (m+6n)^2 + (36n^3-19)/m")
    log(f"Workers: {args.workers}  Target ops/task: {args.target_ops:.2g}")
    log(f"Resuming: pos from {pos},  neg from -{neg+1}")
    log(f"Known solutions: {len(known)}")

    stop = False

    def handler(sig, frame):
        nonlocal stop
        stop = True
        log("Stopping after current batch…")
    signal.signal(signal.SIGINT,  handler)
    signal.signal(signal.SIGTERM, handler)

    t0 = time.time()
    total_searched  = ckpt["total_searched"]
    total_solutions = ckpt["total_solutions"]

    with mp.Pool(processes=args.workers) as pool:
        while not stop:
            # Build batch of 2*workers tasks with adaptive bands
            tasks = []
            batch_n_count = 0
            for _ in range(args.workers):
                if stop:
                    break
                # Adaptive or fixed band
                if args.band:
                    band_p = args.band
                    band_n = args.band
                else:
                    band_p = adaptive_band(max(pos, 1), args.target_ops)
                    band_n = adaptive_band(max(neg, 1), args.target_ops)

                tasks.append((pos, pos + band_p - 1))
                pos += band_p
                batch_n_count += band_p

                tasks.append((-neg - band_n, -neg - 1))
                neg += band_n
                batch_n_count += band_n

            results = pool.map(run_worker_task, tasks)

            all_found = [s for r in results for s in r]
            new = save_solutions(all_found, known)
            total_searched  += batch_n_count
            total_solutions += new

            elapsed = time.time() - t0
            rate    = total_searched / max(elapsed, 1e-9)

            ckpt.update({
                "max_pos": pos, "max_neg": neg,
                "total_searched": total_searched,
                "total_solutions": total_solutions,
            })
            save_ckpt(ckpt)

            for s in all_found:
                log(f"★ SOLUTION: n={s['n']} m={s['m']} Y={s['Y']} X={s['X']}")

            log(f"|n| up to {pos:,}  |  searched {total_searched:,}  |  "
                f"solutions {total_solutions}  |  {rate:.0f} n/s  |  {elapsed:.0f}s")

    log(f"Stopped. Searched: {total_searched:,}  Solutions: {total_solutions}")


if __name__ == "__main__":
    main()
