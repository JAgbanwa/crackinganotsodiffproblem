#!/usr/bin/env python3
"""
assimilator.py  —  BOINC / Charity Engine result assimilator

Reads validated result files and appends verified solutions to
the master solutions file, deduplicating as it goes.

Usage:
    python assimilator.py <result_file> [<result_file> ...]

In BOINC mode, the daemon calls this once per canonical result.
"""

import sys
import os
import re
import json
import logging
import hashlib
from pathlib import Path
from validator import validate_file, verify_solution

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [ASSIM] %(message)s",
    handlers=[logging.StreamHandler(), logging.FileHandler("assimilator.log")]
)
log = logging.getLogger(__name__)

MASTER_FILE   = "solutions_master.txt"
INDEX_FILE    = "solutions_index.json"

# ─────────────────────────────────────────────────────────────────────────────
def load_index() -> set:
    """Load set of known solution fingerprints."""
    if Path(INDEX_FILE).exists():
        with open(INDEX_FILE) as f:
            data = json.load(f)
        return set(tuple(x) for x in data)
    return set()

def save_index(index: set):
    with open(INDEX_FILE, "w") as f:
        json.dump([list(x) for x in sorted(index)], f, indent=2)

def solution_key(n: int, m: int, Y: int) -> tuple:
    """Canonical key — treat +Y and -Y as same solution."""
    return (n, m, abs(Y))

def assimilate_file(path: str, index: set) -> int:
    """Validate file, add new solutions to master. Return count of new."""
    stats  = validate_file(path)
    if stats["invalid"] > 0:
        log.error(f"{path}: {stats['invalid']} INVALID solutions — rejecting file")
        return -1

    new_count = 0
    with open(MASTER_FILE, "a") as f:
        for (n, m, Y, X) in stats["solutions"]:
            key = solution_key(n, m, Y)
            if key in index:
                continue
            index.add(key)
            line = f"n={n} m={m} Y={Y} X={X}\n"
            f.write(line)
            new_count += 1
            log.info(f"NEW SOLUTION: n={n} m={m} Y={Y} X={X}")

    return new_count


# ─────────────────────────────────────────────────────────────────────────────
def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <result_file> [...]", file=sys.stderr)
        sys.exit(1)

    index = load_index()
    total_new = 0

    for path in sys.argv[1:]:
        log.info(f"Processing {path}")
        nc = assimilate_file(path, index)
        if nc < 0:
            log.error(f"Skipped invalid file: {path}")
        else:
            total_new += nc
            log.info(f"{path}: {nc} new solution(s) assimilated")

    save_index(index)
    log.info(f"Done. Total new solutions: {total_new}. "
             f"All-time total: {len(index)}")


if __name__ == "__main__":
    main()
