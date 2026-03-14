#!/usr/bin/env python3
"""
worker.py  —  BOINC / Charity Engine worker (pure Python, arbitrary precision)

Equation:
    Y^2 = (m + 6n)^2 + (36n^3 - 19) / m     (m ≠ 0, m | 36n^3-19)

Usage:
    python worker.py <wu_file> <output_file>

wu_file:  one line: n_start n_end
output:   one solution per line:  n=<n> m=<m> Y=<Y> X=<X>
"""

import sys
import os
import math
import signal
import time

# ── isqrt (Python 3.8+ has math.isqrt) ───────────────────────────────────────
def isqrt(n: int) -> int:
    if n < 0:
        raise ValueError("Square root not defined for negative numbers")
    if n == 0:
        return 0
    return math.isqrt(n)


def is_perfect_square(n: int):
    """Return (True, root) if n is a perfect square, else (False, None)."""
    if n < 0:
        return False, None
    r = isqrt(n)
    if r * r == n:
        return True, r
    return False, None


# ── divisor enumeration ───────────────────────────────────────────────────────
def all_divisors(val: int):
    """
    Yield all non-zero integer divisors (positive and negative) of val.
    """
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


# ── core search ───────────────────────────────────────────────────────────────
def search_range(n_start: int, n_end: int, out, progress_interval: int = 5000):
    """
    Search every n in [n_start, n_end] for integer solutions.
    Writes results to file-like object `out`.
    """
    solutions = []
    t0 = time.time()
    for n in range(n_start, n_end + 1):
        val = 36 * n * n * n - 19
        if val == 0:
            continue  # 36n^3 = 19 has no integer solution anyway

        for m in all_divisors(val):
            quot = val // m                  # exact
            X    = m + 6 * n
            rhs  = X * X + quot

            ok, Y = is_perfect_square(rhs)
            if ok:
                line = f"n={n} m={m} Y={Y} X={X}\n"
                out.write(line)
                out.flush()
                solutions.append((n, m, Y, X))
                if Y != 0:
                    line2 = f"n={n} m={m} Y={-Y} X={X}\n"
                    out.write(line2)
                    out.flush()
                    solutions.append((n, m, -Y, X))

        if (n - n_start) % progress_interval == 0:
            elapsed = time.time() - t0
            rate = (n - n_start + 1) / max(elapsed, 1e-9)
            print(f"[progress] n={n}  elapsed={elapsed:.1f}s  rate={rate:.0f} n/s",
                  flush=True)

    return solutions


# ── main ──────────────────────────────────────────────────────────────────────
def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <wu_file> <output_file>", file=sys.stderr)
        sys.exit(1)

    wu_path  = sys.argv[1]
    out_path = sys.argv[2]

    with open(wu_path) as f:
        parts = f.read().split()
    n_start, n_end = int(parts[0]), int(parts[1])

    print(f"[worker] searching n=[{n_start}, {n_end}]", flush=True)

    with open(out_path, "w") as out:
        search_range(n_start, n_end, out)

    print(f"[worker] done", flush=True)


if __name__ == "__main__":
    main()
