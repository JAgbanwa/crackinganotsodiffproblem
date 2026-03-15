#!/usr/bin/env python3
"""
work_generator.py  —  BOINC / Charity Engine work generator

Generates work units for the search:
    Y^2 = (m + 6n)^2 + (36n^3 - 19) / m     (m ≠ 0, m | 36n^3-19)

Each work unit covers a contiguous range of n values (both positive and negative
are assigned separately). Work units are stored in the BOINC project's
download directory and registered via boinc_submit_work.

Usage (standalone test):
    python work_generator.py --test --n_per_wu 10000 --num_wu 10

In production BOINC mode, this script is called by the BOINC daemon.
"""

import sys
import os
import json
import time
import argparse
import subprocess
import logging
from pathlib import Path

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [WG] %(message)s",
    handlers=[logging.StreamHandler(), logging.FileHandler("work_generator.log")]
)
log = logging.getLogger(__name__)

# ── Configuration ─────────────────────────────────────────────────────────────
BOINC_PROJECT_DIR = os.environ.get("BOINC_PROJECT_DIR", "/home/boincadm/projects/s3c_eq")
DOWNLOAD_DIR      = os.path.join(BOINC_PROJECT_DIR, "download")
STATE_FILE        = "wg_state.json"
WU_NAME_PREFIX    = "s3ceq"
N_PER_WU          = 100_000         # n-values per work unit
                                    # v3: ~4s at 26k n/s (|n|<787k), ~2min at 830 n/s (large n)
MAX_QUEUED_WU     = 500             # keep at most this many open WUs
DEFAULT_APP_NAME  = "s3ceq_worker"

TEMPLATES = {
    "job":    "templates/job.xml",
    "result": "templates/result.xml",
}

# ── State persistence ─────────────────────────────────────────────────────────
def load_state() -> dict:
    if Path(STATE_FILE).exists():
        with open(STATE_FILE) as f:
            return json.load(f)
    return {"next_pos_n": 0, "next_neg_n": -1, "wu_count": 0}

def save_state(state: dict):
    with open(STATE_FILE, "w") as f:
        json.dump(state, f, indent=2)

# ── Work unit creation ────────────────────────────────────────────────────────
def write_wu_input(wu_name: str, n_start: int, n_end: int) -> str:
    """Write work-unit input file; return local path."""
    fname = f"{wu_name}.txt"
    fpath = os.path.join(DOWNLOAD_DIR, fname)
    os.makedirs(DOWNLOAD_DIR, exist_ok=True)
    with open(fpath, "w") as f:
        f.write(f"{n_start} {n_end}\n")
    return fname

def submit_wu_boinc(wu_name: str, input_fname: str,
                    app_name: str = DEFAULT_APP_NAME) -> bool:
    """Submit work unit via create_work BOINC command-line tool."""
    cmd = [
        "bin/create_work",
        "--appname", app_name,
        "--wu_name", wu_name,
        "--wu_template", TEMPLATES["job"],
        "--result_template", TEMPLATES["result"],
        input_fname,
    ]
    try:
        result = subprocess.run(
            cmd, cwd=BOINC_PROJECT_DIR, capture_output=True, text=True
        )
        if result.returncode != 0:
            log.error(f"create_work failed: {result.stderr}")
            return False
        log.info(f"Submitted WU {wu_name} (input: {input_fname})")
        return True
    except FileNotFoundError:
        log.warning("create_work not found — running in test/stub mode")
        return True  # stub success in test mode

def create_work_unit(state: dict, n_start: int, n_end: int,
                     direction: str = "pos") -> bool:
    """Create one work unit for n in [n_start, n_end]."""
    wuid      = state["wu_count"] + 1
    wu_name   = f"{WU_NAME_PREFIX}_{direction}_{wuid:010d}"
    input_f   = write_wu_input(wu_name, n_start, n_end)
    ok        = submit_wu_boinc(wu_name, input_f)
    if ok:
        state["wu_count"] = wuid
    return ok

# ── Main generation loop ──────────────────────────────────────────────────────
def run_generator(n_per_wu: int, max_queued: int, test_mode: bool,
                  num_wu_test: int):
    state = load_state()
    log.info(f"Work generator started. next_pos_n={state['next_pos_n']}, "
             f"next_neg_n={state['next_neg_n']}, wu_count={state['wu_count']}")

    iterations = 0
    while True:
        # Alternate: positive then negative n bands ──────────────────────────
        # Positive band
        ps = state["next_pos_n"]
        pe = ps + n_per_wu - 1
        create_work_unit(state, ps, pe, direction="pos")
        state["next_pos_n"] = pe + 1
        save_state(state)

        # Negative band
        ns = state["next_neg_n"] - n_per_wu + 1
        ne = state["next_neg_n"]
        create_work_unit(state, ns, ne, direction="neg")
        state["next_neg_n"] = ns - 1
        save_state(state)

        log.info(f"WUs created: {state['wu_count']} | "
                 f"pos up to {pe} | neg down to {ns}")

        iterations += 1
        if test_mode and iterations >= num_wu_test // 2:
            log.info("Test mode: reached requested number of WUs.")
            break

        if not test_mode:
            time.sleep(5)   # throttle: don't flood BOINC scheduler


# ── Entry point ───────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="BOINC work generator for s3ceq search")
    parser.add_argument("--n_per_wu", type=int, default=N_PER_WU,
                        help="n-values per work unit")
    parser.add_argument("--max_queued", type=int, default=MAX_QUEUED_WU)
    parser.add_argument("--test", action="store_true",
                        help="Test mode: create a few WUs and exit")
    parser.add_argument("--num_wu", type=int, default=10,
                        help="How many WUs to create in test mode")
    args = parser.parse_args()

    run_generator(args.n_per_wu, args.max_queued, args.test, args.num_wu)


if __name__ == "__main__":
    main()
