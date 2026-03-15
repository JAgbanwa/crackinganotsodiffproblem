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
_TEST_MODE        = False   # set True in main() when --test is given

TEMPLATES = {
    "job":    "templates/job.xml",
    "result": "templates/result.xml",
}

# ── Estimate which n range covers a given |y| value ──────────────────────────
def n_range_for_y(y_target: int, headroom: float = 2.0):
    """
    Return (n_lo, n_hi) such that val = 36n^3-19 is in [y_target/headroom, y_target*headroom].
    i.e. the n range where divisors of val can be ~ y_target.
    Solves n = ((y+19)/36)^(1/3), rounded to nearest decade.
    """
    import math
    n_center = ((y_target + 19) / 36) ** (1/3)
    n_lo = int(n_center / headroom ** (1/3))
    n_hi = int(n_center * headroom ** (1/3)) + 1
    return n_lo, n_hi

# ── State persistence ─────────────────────────────────────────────────────────
def load_state(start_pos: int = 0, start_neg: int = -1,
               bands: list | None = None) -> dict:
    """
    Load or initialise state.  bands is a list of (lo, hi) integer pairs;
    when supplied the state tracks each band independently and interleaves WUs.
    A fresh state is created when the state file does not exist.
    Explicit start_pos / start_neg override the stored values (one-time jump).
    """
    if Path(STATE_FILE).exists():
        with open(STATE_FILE) as f:
            state = json.load(f)
    else:
        state = {"wu_count": 0}

    if bands:
        # Multi-band mode — each band has its own cursor
        existing = {b["key"]: b for b in state.get("bands", [])}
        new_bands = []
        for lo, hi in bands:
            key = f"{lo}:{hi}"
            if key in existing:
                new_bands.append(existing[key])
            else:
                new_bands.append({"key": key, "lo": lo, "hi": hi,
                                   "next_pos": lo, "next_neg": -lo - 1})
        state["bands"] = new_bands
        state.setdefault("band_idx", 0)
    else:
        # Legacy single-range mode
        if "next_pos_n" not in state:
            state["next_pos_n"] = start_pos
        elif start_pos != 0:
            state["next_pos_n"] = start_pos
        if "next_neg_n" not in state:
            state["next_neg_n"] = start_neg
        elif start_neg != -1:
            state["next_neg_n"] = start_neg
    return state

def save_state(state: dict):
    with open(STATE_FILE, "w") as f:
        json.dump(state, f, indent=2)

# ── Work unit creation ────────────────────────────────────────────────────────
def write_wu_input(wu_name: str, n_start: int, n_end: int,
                   test_mode: bool = False) -> str:
    """Write work-unit input file; return local filename."""
    fname = f"{wu_name}.txt"
    outdir = "/tmp/s3ceq_test_wu" if test_mode else DOWNLOAD_DIR
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, fname), "w") as f:
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
    input_f   = write_wu_input(wu_name, n_start, n_end, test_mode=_TEST_MODE)
    ok        = submit_wu_boinc(wu_name, input_f)
    if ok:
        state["wu_count"] = wuid
    return ok

# ── Main generation loop ──────────────────────────────────────────────────────
def run_generator(n_per_wu: int, max_queued: int, test_mode: bool,
                  num_wu_test: int, start_pos: int = 0, start_neg: int = -1,
                  bands: list | None = None):
    """
    bands: list of (lo, hi) n-ranges to interleave.  When None, use single
    range starting at start_pos / start_neg (legacy mode).
    """
    state = load_state(start_pos, start_neg, bands)

    if bands:
        log.info(f"Multi-band mode: {len(state['bands'])} bands, "
                 f"wu_count={state['wu_count']}")
        for b in state["bands"]:
            log.info(f"  band [{b['lo']}, {b['hi']}]: next_pos={b['next_pos']}, "
                     f"next_neg={b['next_neg']}")
    else:
        log.info(f"Work generator started. next_pos_n={state['next_pos_n']}, "
                 f"next_neg_n={state['next_neg_n']}, wu_count={state['wu_count']}")

    iterations = 0
    while True:
        if bands:
            # ── Multi-band interleaved mode ───────────────────────────────
            band_list = state["bands"]
            bi = state["band_idx"] % len(band_list)
            b  = band_list[bi]

            # Positive slice of this band
            ps = b["next_pos"]
            pe = ps + n_per_wu - 1
            if pe > b["hi"]:
                pe = b["hi"]
            if ps <= b["hi"]:
                create_work_unit(state, ps, pe,
                                 direction=f"pos_b{bi}")
                b["next_pos"] = pe + 1
                save_state(state)

            # Negative slice (mirror of band)
            ns = b["next_neg"] - n_per_wu + 1
            ne = b["next_neg"]
            lo_neg = -(b["hi"] + 1)
            if ne >= lo_neg:
                create_work_unit(state, ns, ne,
                                 direction=f"neg_b{bi}")
                b["next_neg"] = ns - 1
                save_state(state)

            state["band_idx"] = (bi + 1) % len(band_list)
            log.info(f"WUs: {state['wu_count']} | band[{bi}] "
                     f"pos={b['next_pos']} neg={b['next_neg']}")
        else:
            # ── Legacy single-range mode ──────────────────────────────────
            ps = state["next_pos_n"]
            pe = ps + n_per_wu - 1
            create_work_unit(state, ps, pe, direction="pos")
            state["next_pos_n"] = pe + 1
            save_state(state)

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
        description="BOINC work generator for s3ceq search",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Normal sequential search from n=0:
  python work_generator.py --test --num_wu 20

  # Jump the positive search to n=10^11 (target y ~ 10^35 divisors):
  python work_generator.py --start_n 100000000000 --test --num_wu 20

  # Multi-band: interleave small-n (0..10^6) WITH large-n (10^11..10^12):
  python work_generator.py --bands 0:1000000,100000000000:1000000000000 --test --num_wu 20

  # Target a specific |y| value — auto-compute the right n range:
  python work_generator.py --target_y 1000000000000000000000000000000000  # 10^33
""")
    parser.add_argument("--n_per_wu", type=int, default=N_PER_WU,
                        help="n-values per work unit (default: %(default)s)")
    parser.add_argument("--max_queued", type=int, default=MAX_QUEUED_WU)
    parser.add_argument("--test", action="store_true",
                        help="Test mode: create a few WUs and exit")
    parser.add_argument("--num_wu", type=int, default=10,
                        help="WUs to create in test mode (default: %(default)s)")
    parser.add_argument("--start_n", type=int, default=0,
                        help="Bootstrap positive-n cursor to this value (overrides state file)")
    parser.add_argument("--neg_start_n", type=int, default=-1,
                        help="Bootstrap negative-n cursor (default: -1)")
    parser.add_argument("--bands", type=str, default=None,
                        help="Comma-separated list of lo:hi n-ranges to search in parallel, "
                             "e.g. 0:1000000,100000000000:1000000000000")
    parser.add_argument("--target_y", type=int, default=None,
                        help="Auto-set --bands for the n range covering divisors |y| ~ target_y")
    args = parser.parse_args()

    global _TEST_MODE
    _TEST_MODE = args.test

    # Resolve --target_y → --bands
    if args.target_y is not None:
        lo, hi = n_range_for_y(args.target_y)
        log.info(f"--target_y {args.target_y:.3e}: mapped to n range [{lo:,}, {hi:,}]")
        args.bands = f"{lo}:{hi}"

    # Parse --bands string → list of (lo, hi)
    parsed_bands = None
    if args.bands:
        try:
            parsed_bands = [
                tuple(int(x) for x in seg.split(":"))
                for seg in args.bands.split(",")
            ]
        except ValueError:
            parser.error("--bands must be comma-separated lo:hi integer pairs")
        log.info(f"Bands: {parsed_bands}")

    run_generator(args.n_per_wu, args.max_queued, args.test, args.num_wu,
                  start_pos=args.start_n, start_neg=args.neg_start_n,
                  bands=parsed_bands)


if __name__ == "__main__":
    main()
