/*
 * worker.c  —  Charity Engine / BOINC worker
 *
 * Searches for ALL integer solutions (n, m, Y) satisfying:
 *
 *     Y^2 = (m + 6n)^2 + (36n^3 - 19) / m     (m ≠ 0, m | 36n^3-19)
 *
 * Equivalently, with X = m + 6n:
 *     Y^2 - X^2 = (36n^3 - 19) / m
 *     (Y-X)(Y+X) = quotient
 *
 * Strategy per n:
 *   1. val = 36*n^3 - 19
 *   2. Enumerate all integer divisors m of val
 *   3. For each m: rhs = (m+6n)^2 + val/m; check if rhs is a perfect square
 *
 * Uses __int128 for intermediate arithmetic to avoid overflow for large |n|.
 * For |n| up to ~10^12 the intermediate values fit in __int128 (16 bytes).
 *
 * Usage:  ./worker <wu_file> <output_file>
 *
 * wu_file format (one line):
 *     n_start n_end
 *
 * output_file: one solution per line: n m Y
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

typedef __int128  i128;
typedef int64_t   i64;

/* ------------------------------------------------------------------ */
/* Integer square root of a non-negative __int128; returns floor(sqrt) */
static i128 isqrt128(i128 n) {
    if (n < 0) return -1;
    if (n == 0) return 0;
    /* Newton's method starting from 64-bit sqrt */
    i128 x = (i128)1 << 63;
    /* Reduce starting estimate */
    while (x * x > n) x >>= 1;
    /* Refine upward */
    i128 x1 = (x + n / x) / 2;
    while (x1 < x) {
        x  = x1;
        x1 = (x + n / x) / 2;
    }
    return x;
}

/* Check if v is a perfect square; if yes set *root = sqrt(v), return 1 */
static int is_perfect_square(i128 v, i128 *root) {
    if (v < 0) return 0;
    i128 r = isqrt128(v);
    if (r * r == v) { *root = r; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Enumerate all positive divisors of |val|; store in buf; return count */
static int pos_divisors(i128 val, i128 *buf) {
    if (val < 0) val = -val;
    if (val == 0) return 0;
    int cnt = 0;
    i128 tmp[2048];
    for (i128 d = 1; d * d <= val; d++) {
        if (val % d == 0) {
            tmp[cnt++] = d;
            if (d != val / d)
                tmp[cnt++] = val / d;
        }
    }
    /* Copy positive and negative into buf */
    int total = 0;
    for (int i = 0; i < cnt; i++) {
        buf[total++] = tmp[i];
        buf[total++] = -tmp[i];
    }
    return total;
}

/* ------------------------------------------------------------------ */
static void print128(FILE *fp, i128 v) {
    if (v < 0) { fprintf(fp, "-"); v = -v; }
    if (v > 9) print128(fp, v / 10);
    fputc('0' + (int)(v % 10), fp);
}

/* ------------------------------------------------------------------ */
static void search_range(i64 n_start, i64 n_end, FILE *out, FILE *log) {
    i128 div_buf[65536];

    for (i64 n = n_start; n <= n_end; n++) {
        i128 n128 = (i128)n;
        i128 val  = (i128)36 * n128 * n128 * n128 - 19;

        if (val == 0) {
            /* 36n^3 = 19 has no integer solution, but guard anyway */
            continue;
        }

        int ndiv = pos_divisors(val, div_buf);

        for (int d = 0; d < ndiv; d++) {
            i128 m = div_buf[d];
            if (m == 0) continue;

            i128 quot = val / m;           /* exact integer division */
            i128 X    = m + (i128)6 * n128;
            i128 rhs  = X * X + quot;

            i128 Y;
            if (is_perfect_square(rhs, &Y)) {
                /* Solution found: record (n, m, Y) and also (n, m, -Y) */
                fprintf(out, "n=");
                print128(out, n128);
                fprintf(out, " m=");
                print128(out, m);
                fprintf(out, " Y=");
                print128(out, Y);
                fprintf(out, " X=");
                print128(out, X);
                fprintf(out, "\n");
                fflush(out);

                if (Y != 0) {
                    fprintf(out, "n=");
                    print128(out, n128);
                    fprintf(out, " m=");
                    print128(out, m);
                    fprintf(out, " Y=");
                    print128(out, -Y);
                    fprintf(out, " X=");
                    print128(out, X);
                    fprintf(out, "\n");
                    fflush(out);
                }
                if (log) {
                    fprintf(log, "[SOLUTION] n=%lld m=", (long long)n);
                    print128(log, m);
                    fprintf(log, " Y=");
                    print128(log, Y);
                    fprintf(log, "\n");
                    fflush(log);
                }
            }
        }

        /* Progress heartbeat every 10000 n values */
        if ((n & 0x2710) == 0 && log) {
            fprintf(log, "[progress] n=%lld\n", (long long)n);
            fflush(log);
        }
    }
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <wu_file> <output_file>\n", argv[0]);
        return 1;
    }

    FILE *wu = fopen(argv[1], "r");
    if (!wu) { perror("open wu_file"); return 1; }

    int64_t n_start, n_end;
    if (fscanf(wu, "%" SCNd64 " %" SCNd64, &n_start, &n_end) != 2) {
        fprintf(stderr, "Bad wu_file format (expect: n_start n_end)\n");
        fclose(wu);
        return 1;
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
