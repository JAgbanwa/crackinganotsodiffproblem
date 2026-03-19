# Search Progress Log

**Equation being solved:**

$$Y^2 = (m + 6n)^2 + \frac{36n^3 - 19}{m}, \quad m \neq 0,\; m \mid 36n^3-19$$

**Ultimate goal:** Find integer triples $(x, y, z)$ satisfying $x^3 + y^3 + z^3 = 114$.  
Every integer solution $(m, n, Y)$ to the equation above directly yields such a triple via the parametric construction in [this Figshare preprint](https://figshare.com/articles/preprint/Closed_form_formulas_on_the_sums_of_three_cubes_for_k_114_192_/30509981).  
114 is the smallest integer not yet known to be representable as a sum of three cubes.

---

## Confirmed Negative Ranges

| Range | Method | Date | Solutions found |
|-------|--------|------|----------------|
| $0 \leq n \leq 787{,}000$ | 64-bit Pollard-rho (single core) | 2026-03-19 | **0** |
| $-787{,}000 \leq n \leq -1$ | 64-bit Pollard-rho (single core) | 2026-03-19 | **0** |
| $787{,}001 \leq \|n\| \leq 800{,}000$ | 128-bit Pollard-rho (4 cores) | 2026-03-19 | **0** |

**Total integer-n values verified negative:** $1{,}600{,}000$ (i.e. $|n| \leq 800{,}000$)

---

## Solutions Found

*None yet.*

---

## Search Infrastructure

### Local Machine (macOS, 4–8 cores)

| $|n|$ regime | Algorithm | Speed (4 cores) |
|---|---|---|
| $\leq 787{,}000$ | 64-bit Pollard-rho + Miller-Rabin | ~60,000 n/s |
| $787{,}001 – 2.1 \times 10^{12}$ | 128-bit Pollard-rho + Miller-Rabin | ~3,300 n/s |

To resume local search:
```bash
make all
python3 fast_local_search.py --workers 4
```
The search auto-resumes from `checkpoint_fast.json`.

---

### Charity Engine (BOINC, ~500,000 volunteer cores)

| Range | Projected wall-clock time |
|-------|--------------------------|
| $\|n\| \leq 787{,}000$ | < 1 second |
| $\|n\| \leq 2.1 \times 10^{12}$ (full v3 ceiling) | ~2.5 hours |

Deployment status:
- [x] Worker binary (`worker_s3ceq_linux`, Linux x86_64 static) — ready
- [x] BOINC server Docker stack — ready (`docker-compose.yml`)
- [x] Work generator, validator, assimilator daemons — ready
- [ ] VPS provisioned — **pending**
- [ ] Charity Engine project application — **pending** (apply at https://www.charityengine.com/apply-for-computing-power/)

Once the CE project is approved and the VPS is live, all work units are automatically generated and fanned out to volunteer machines. Results flow back, are verified, and appended to `solutions_master.txt`.

---

## Modular Sieves (pre-rejection before factorisation)

The v4 worker applies two layers of modular arithmetic to skip unpromising $n$ values:

| Layer | Primes | Rejection rate |
|-------|--------|---------------|
| Outer n-sieve | 7, 11, 13 | ~20–25% of all n |
| Inner QR bitmask | mod 64, 45, 7, 11 | ~98% of remaining divisors |

Combined, only ~1.6% of divisor candidates reach the full `isqrt` check.

---

## How a Solution Maps to $x^3 + y^3 + z^3 = 114$

Given integers $(m, n, Y)$ satisfying the equation, set:

$$X = m + 6n$$

Then $(x, y, z)$ can be recovered as a rational triple and, if $m \mid 1$ conditions are met, as an integer triple solving $x^3 + y^3 + z^3 = 114$.

See [validator.py](validator.py) for the full algebraic check.

---

*Last updated: 2026-03-19 — local search running on macOS (4 workers), currently at $|n| \approx 800{,}000$.*
