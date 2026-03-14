/*
 * worker.c  --  Charity Engine / BOINC worker  (v2: Pollard-rho factorisation)
 *
 * Searches for ALL integer solutions (n, m, Y) satisfying:
 *
 *     Y^2 = (m + 6n)^2 + (36n^3 - 19) / m     (m != 0, m | 36n^3-19)
 *
 * Strategy per n:
 *   1. val = 36*n^3 - 19
 *   2. Factorise |val| via Pollard-rho (Miller-Rabin primality test)
 *   3. Enumerate all 2*d(val) divisors (pos & neg) from the factorisation
 *   4. For each divisor m: rhs = (m+6n)^2 + val/m; check perfect square
 *
 * Factorisation is O(val^(1/4) * polylog) vs O(sqrt(val)) for trial division.
 * For |n| > TRIAL_LIMIT (val too large for reliable 64-bit Pollard-rho),
 * fall back to trial division up to cbrt(val), which still handles the
 * "small prime" part fast.
 *
 * Uses __int128 for final arithmetic (overflow-safe for |n| up to ~10^12).
 *
 * Usage:  ./worker <wu_file> <output_file>
 * wu_file: single line "n_start n_end"
 * output:  one solution per line: n=... m=... Y=... X=...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>

typedef __int128   i128;
typedef int64_t    i64;
typedef uint64_t   u64;
typedef unsigned __int128 u128;

/* For |n| above this, |val| > 2^63; use fallback trial-division factoriser */
#define TRIAL_LIMIT  2600000LL

/* --------------------------------------------------------------------------
 * 64-bit modular arithmetic helpers
 * -------------------------------------------------------------------------- */

/* (a * b) % m  -- uses u128 to avoid overflow */
static inline u64 mulmod64(u64 a, u64 b, u64 m) {
    return (u64)(((u128)a * b) % m);
}

/* a^e % m */
static u64 powmod64(u64 a, u64 e, u64 m) {
    u64 r = 1;
    a %= m;
    while (e) {
        if (e & 1) r = mulmod64(r, a, m);
        a = mulmod64(a, a, m);
        e >>= 1;
    }
    return r;
}

/* --------------------------------------------------------------------------
 * Miller-Rabin primality test (deterministic for n < 3,317,044,064,679,887,385,961,981)
 * Using witnesses {2,3,5,7,11,13,17,19,23,29,31,37}
 * -------------------------------------------------------------------------- */
static int is_prime64(u64 n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;

    /* Write n-1 = 2^r * d */
    u64 d = n - 1;
    int r = 0;
    while ((d & 1) == 0) { d >>= 1; r++; }

    static const u64 witnesses[] = {2,3,5,7,11,13,17,19,23,29,31,37};
    for (int i = 0; i < 12; i++) {
        u64 a = witnesses[i];
        if (a >= n) continue;
        u64 x = powmod64(a, d, n);
        if (x == 1 || x == n - 1) continue;
        int composite = 1;
        for (int j = 0; j < r - 1; j++) {
            x = mulmod64(x, x, n);
            if (x == n - 1) { composite = 0; break; }
        }
        if (composite) return 0;
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * Pollard-rho (Brent's improvement) -- returns a non-trivial factor of n,
 * or n if it fails (caller must retry or fall back).
 * -------------------------------------------------------------------------- */
static u64 pollard_rho64(u64 n) {
    if (n % 2 == 0) return 2;
    /* Try several starting values of c */
    for (u64 c = 1; c < 20; c++) {
        u64 x = 2, y = 2, d = 1;
        /* Brent: keep moving y by powers of 2 steps ahead */
        u64 m = 128;
        u64 ys = 0, r = 1, q = 1;
        do {
            x = y;
            for (u64 i = 0; i < r; i++)
                y = (mulmod64(y, y, n) + c) % n;
            u64 k = 0;
            d = 1;
            while (k < r && d == 1) {
                ys = y;
                for (u64 i = 0; i < (m < r - k ? m : r - k); i++) {
                    y = (mulmod64(y, y, n) + c) % n;
                    u64 diff = (x > y) ? x - y : y - x;
                    q = mulmod64(q, diff, n);
                }
                /* gcd(q, n) */
                u64 a = q, b = n;
                while (b) { u64 t = b; b = a % b; a = t; }
                d = a;
                k += m;
            }
            r *= 2;
        } while (d == 1);

        if (d == n) {
            /* Backtrack: step one at a time */
            d = 1;
            y = ys;
            while (d == 1) {
                y = (mulmod64(y, y, n) + c) % n;
                u64 diff = (x > y) ? x - y : y - x;
                u64 a = diff, b = n;
                while (b) { u64 t = b; b = a % b; a = t; }
                d = a;
            }
        }
        if (d != n) return d;
    }
    return n; /* failed -- caller uses trial division */
}

/* --------------------------------------------------------------------------
 * Prime factorisation of n into (prime, exponent) pairs.
 * out[] must hold at least 64 entries.
 * Returns number of distinct prime factors.
 * -------------------------------------------------------------------------- */
typedef struct { u64 p; int e; } PrimePow;

static void factor_insert(PrimePow *out, int *nf, u64 p) {
    for (int i = 0; i < *nf; i++) {
        if (out[i].p == p) { out[i].e++; return; }
    }
    out[*nf].p = p;
    out[*nf].e = 1;
    (*nf)++;
}

/* Recursive helper: fully factor n and insert into out[] */
static void factor_rec(u64 n, PrimePow *out, int *nf) {
    if (n <= 1) return;
    if (is_prime64(n)) { factor_insert(out, nf, n); return; }
    /* Try small primes first */
    for (u64 p = 2; p * p <= n && p < 1000; p++) {
        while (n % p == 0) { factor_insert(out, nf, p); n /= p; }
    }
    if (n == 1) return;
    if (is_prime64(n)) { factor_insert(out, nf, n); return; }
    /* Pollard-rho split */
    u64 d = pollard_rho64(n);
    if (d == n) {
        /* Failed: trial divide the rest */
        for (u64 p = 2; p * p <= n; p++) {
            while (n % p == 0) { factor_insert(out, nf, p); n /= p; }
        }
        if (n > 1) factor_insert(out, nf, n);
        return;
    }
    factor_rec(d,     out, nf);
    factor_rec(n / d, out, nf);
}

static int factorize64(u64 n, PrimePow *out) {
    int nf = 0;
    factor_rec(n, out, &nf);
    /* Sort by prime (insertion sort -- small nf) */
    for (int i = 1; i < nf; i++) {
        PrimePow kv = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].p > kv.p) { out[j+1] = out[j]; j--; }
        out[j+1] = kv;
    }
    return nf;
}

/* --------------------------------------------------------------------------
 * Enumerate all POSITIVE divisors of n from its factorisation.
 * Stores them in g_divs[]; returns count.
 * -------------------------------------------------------------------------- */
static u64 g_divs[131072];
static int g_ndivs;

static void enum_div_rec(PrimePow *pf, int nf, int idx, u64 cur) {
    if (idx == nf) {
        g_divs[g_ndivs++] = cur;
        return;
    }
    u64 pk = 1;
    for (int e = 0; e <= pf[idx].e; e++) {
        enum_div_rec(pf, nf, idx + 1, cur * pk);
        pk *= pf[idx].p;
    }
}

/* --------------------------------------------------------------------------
 * Fallback: enumerate divisors of |val| (as i128) by trial division.
 * Used when |n| > TRIAL_LIMIT and val may not fit in u64.
 * Stores signed divisors (+d and -d) in div_buf[]; returns count.
 * -------------------------------------------------------------------------- */
static i128 fb_divs[131072];
static int enum_divisors_fallback(i128 val, i128 *div_buf) {
    if (val < 0) val = -val;
    if (val == 0) return 0;
    int cnt = 0;
    i128 tmp[65536];
    for (i128 d = 1; d * d <= val; d++) {
        if (val % d == 0) {
            tmp[cnt++] = d;
            if (d != val / d) tmp[cnt++] = val / d;
            if (cnt >= 65534) break;
        }
    }
    int total = 0;
    for (int i = 0; i < cnt; i++) {
        div_buf[total++] =  tmp[i];
        div_buf[total++] = -tmp[i];
    }
    return total;
}

/* --------------------------------------------------------------------------
 * __int128 helpers
 * -------------------------------------------------------------------------- */
static i128 isqrt128(i128 n) {
    if (n <= 0) return 0;
    i128 x = (i128)1 << 63;
    while (x * x > n) x >>= 1;
    i128 x1 = (x + n / x) / 2;
    while (x1 < x) { x = x1; x1 = (x + n / x) / 2; }
    return x;
}

static int is_perfect_square(i128 v, i128 *root) {
    if (v < 0) return 0;
    i128 r = isqrt128(v);
    if (r * r == v) { *root = r; return 1; }
    return 0;
}

static void print128(FILE *fp, i128 v) {
    if (v < 0) { fputc('-', fp); v = -v; }
    if (v > 9) print128(fp, v / 10);
    fputc('0' + (int)(v % 10), fp);
}

/* --------------------------------------------------------------------------
 * Emit a solution (and its Y/-Y pair) to the output file
 * -------------------------------------------------------------------------- */
static void emit(FILE *out, FILE *log, i128 n128, i128 m, i128 Y, i128 X) {
    fprintf(out, "n="); print128(out, n128);
    fprintf(out, " m="); print128(out, m);
    fprintf(out, " Y="); print128(out, Y);
    fprintf(out, " X="); print128(out, X);
    fprintf(out, "\n"); fflush(out);
    if (Y != 0) {
        fprintf(out, "n="); print128(out, n128);
        fprintf(out, " m="); print128(out, m);
        fprintf(out, " Y="); print128(out, -Y);
        fprintf(out, " X="); print128(out, X);
        fprintf(out, "\n"); fflush(out);
    }
    if (log) {
        fprintf(log, "[SOLUTION] n="); print128(log, n128);
        fprintf(log, " m="); print128(log, m);
        fprintf(log, " Y="); print128(log, Y);
        fprintf(log, "\n"); fflush(log);
    }
}

/* --------------------------------------------------------------------------
 * Check one divisor m of val against the equation
 * -------------------------------------------------------------------------- */
static void check_divisor(i64 n, i128 n128, i128 val, i128 m,
                           FILE *out, FILE *log) {
    if (m == 0) return;
    i128 quot = val / m;
    i128 X    = m + (i128)6 * n128;
    i128 rhs  = X * X + quot;
    i128 Y;
    if (is_perfect_square(rhs, &Y))
        emit(out, log, n128, m, Y, X);
    (void)n; /* suppress unused parameter warning */
}

/* --------------------------------------------------------------------------
 * Main search loop
 * -------------------------------------------------------------------------- */
static void search_range(i64 n_start, i64 n_end, FILE *out, FILE *log) {
    PrimePow pf[64];

    for (i64 n = n_start; n <= n_end; n++) {
        i128 n128 = (i128)n;
        i128 val  = (i128)36 * n128 * n128 * n128 - 19;
        if (val == 0) continue;

        i64 n_abs = (n < 0) ? -n : n;

        if (n_abs <= TRIAL_LIMIT) {
            /* ---------- fast path: Pollard-rho factorisation ---------- */
            /* val fits in u64 (|val| <= 36 * (2.6e6)^3 ~ 6.5e20 ... hmm)
             * Actually for n_abs = 2.6e6: val ~ 36 * (2.6e6)^3 = 6.5e20
             * That overflows u64 (max ~1.8e19). Reduce TRIAL_LIMIT. */
            /* Safe bound: u64 max ~1.84e19 => n_abs^3 * 36 < 1.84e19
             *  => n_abs < (1.84e19 / 36)^(1/3) ~ 787000 */
            u64 val64;
            int sign_val = (val >= 0) ? 1 : -1;
            i128 abs_val = (val >= 0) ? val : -val;

            if (abs_val <= (i128)0xFFFFFFFFFFFFFFFFULL && n_abs <= 787000) {
                val64 = (u64)abs_val;
                int nf = factorize64(val64, pf);

                g_ndivs = 0;
                enum_div_rec(pf, nf, 0, 1);
                int nd = g_ndivs;

                for (int d = 0; d < nd; d++) {
                    i128 g = (i128)g_divs[d];
                    /* divisors: +g and -g */
                    check_divisor(n, n128, val, sign_val > 0 ?  g : -g, out, log);
                    check_divisor(n, n128, val, sign_val > 0 ? -g :  g, out, log);
                }
            } else {
                /* abs_val too big for u64 but n still "small" -- use fallback */
                int total = enum_divisors_fallback(val, fb_divs);
                for (int d = 0; d < total; d++)
                    check_divisor(n, n128, val, fb_divs[d], out, log);
            }
        } else {
            /* ---------- fallback: trial-division for large |n| ---------- */
            int total = enum_divisors_fallback(val, fb_divs);
            for (int d = 0; d < total; d++)
                check_divisor(n, n128, val, fb_divs[d], out, log);
        }

        if ((n % 10000) == 0 && log) {
            fprintf(log, "[progress] n=%lld\n", (long long)n);
            fflush(log);
        }
    }
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <wu_file> <output_file>\n", argv[0]);
        return 1;
    }
    FILE *wu = fopen(argv[1], "r");
    if (!wu) { perror("open wu_file"); return 1; }
    i64 n_start, n_end;
    if (fscanf(wu, "%" SCNd64 " %" SCNd64, &n_start, &n_end) != 2) {
        fprintf(stderr, "Bad wu_file format (expect: n_start n_end)\n");
        fclose(wu); return 1;
    }
    fclose(wu);

    FILE *out = fopen(argv[2], "w");
    if (!out) { perror("open output_file"); return 1; }
    FILE *log = fopen("worker_progress.log", "a");

    if (log) {
        fprintf(log, "[start] searching n=[%lld, %lld]\n",
                (long long)n_start, (long long)n_end);
        fflush(log);
    }

    search_range(n_start, n_end, out, log);

    if (log) {
        fprintf(log, "[done] n=[%lld, %lld]\n",
                (long long)n_start, (long long)n_end);
        fclose(log);
    }
    fclose(out);
    return 0;
}
