#!/usr/bin/env python3
"""
validator.py  —  BOINC result validator

Verifies that reported solutions truly satisfy:
    Y^2 = (m + 6n)^2 + (36n^3 - 19) / m     (m ≠ 0, m | 36n^3-19)

BOINC canonical result selection: if ≥2 results agree on the set of solutions
in the same n-range, they are considered canonical.

Usage (standalone):
    python validator.py <result_file>
"""

import sys
import re
import math
import logging
from pathlib import Path

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [VAL] %(message)s",
    handlers=[logging.StreamHandler(), logging.FileHandler("validator.log")]
)
log = logging.getLogger(__name__)

SOLUTION_RE = re.compile(
    r"n=(-?\d+)\s+m=(-?\d+)\s+Y=(-?\d+)\s+X=(-?\d+)"
)

# ─────────────────────────────────────────────────────────────────────────────
def verify_solution(n: int, m: int, Y: int, X: int) -> bool:
    """Return True iff (n, m, Y, X) satisfies all constraints."""
    # 1. X = m + 6n
    if X != m + 6 * n:
        log.warning(f"X mismatch: n={n} m={m} X={X} expected={m+6*n}")
        return False
    # 2. m must divide 36n^3 - 19
    val = 36 * n**3 - 19
    if val % m != 0:
        log.warning(f"m does not divide val: n={n} m={m} val={val}")
        return False
    # 3. RHS must equal Y^2
    quot = val // m
    rhs  = X * X + quot
    if Y * Y != rhs:
        log.warning(f"Y^2 ≠ RHS: n={n} m={m} Y={Y} rhs={rhs}")
        return False
    return True


def validate_file(path: str) -> dict:
    """
    Parse and validate all solutions in a result file.
    Returns dict with keys: valid, invalid, solutions.
    """
    stats = {"valid": 0, "invalid": 0, "solutions": []}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                m = SOLUTION_RE.search(line)
                if not m:
                    continue
                n, m_val, Y, X = (int(m.group(i)) for i in range(1, 5))
                ok = verify_solution(n, m_val, Y, X)
                if ok:
                    stats["valid"] += 1
                    stats["solutions"].append((n, m_val, Y, X))
                    log.info(f"VALID: n={n} m={m_val} Y={Y} X={X}")
                else:
                    stats["invalid"] += 1
                    log.error(f"INVALID: n={n} m={m_val} Y={Y} X={X}")
    except FileNotFoundError:
        log.error(f"File not found: {path}")
    return stats


def results_match(file1: str, file2: str) -> bool:
    """Return True if both result files contain the same verified solutions."""
    s1 = validate_file(file1)
    s2 = validate_file(file2)
    set1 = set(s1["solutions"])
    set2 = set(s2["solutions"])
    match = (set1 == set2) and s1["invalid"] == 0 and s2["invalid"] == 0
    if not match:
        log.warning(f"Results differ: {len(set1)} vs {len(set2)} solutions")
    return match


# ─────────────────────────────────────────────────────────────────────────────
def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <result_file>", file=sys.stderr)
        sys.exit(1)
    stats = validate_file(sys.argv[1])
    print(f"Valid solutions:   {stats['valid']}")
    print(f"Invalid solutions: {stats['invalid']}")
    for sol in stats["solutions"]:
        print(f"  n={sol[0]}  m={sol[1]}  Y={sol[2]}  X={sol[3]}")
    sys.exit(0 if stats["invalid"] == 0 else 1)


if __name__ == "__main__":
    main()
