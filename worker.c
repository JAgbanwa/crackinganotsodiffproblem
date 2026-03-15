/*
 * worker.c  --  Charity Engine / BOINC worker  (v3: 128-bit Pollard-rho)
 *
 * Searches for ALL integer solutions (n, m, Y) satisfying:
 *
 *     Y^2 = (m + 6n)^2 + (36n^3 - 19) / m     (m != 0, m | 36n^3-19)
 *
 * Strategy per n:
 *   1. val = 36*n^3 - 19
 *   2. Factorise |val| via 128-bit Pollard-rho (Miller-Rabin primality test)
 *   3. Enumerate all 2*d(val) divisors (pos & neg) from the factorisation
 *   4. For each divisor m: rhs = (m+6n)^2 + val/m; check perfect square
 *
 * v3 changes vs v2:
 *   - Full 128-bit Pollard-rho (mulmod128 via binary method).
 *   - No 64-bit ceiling: fast path covers all |n| <= TRIAL_LIMIT_128 = 2.1e12.
 *   - For |n| > 2.1e12 fall back to trial division (very rarely needed with CE).
 *   - Overflow guard: skip divisors where (m+6n)^2 would exceed i128.
 *
 * Uses __int128 / unsigned __int128 throughout (GCC/Clang extension).
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

typedef __int128          i128;
typedef int64_t           i64;
typedef uint64_t          u64;
typedef unsigned __int128 u128;

/*
 * For |n| up to TRIAL_LIMIT_128 the 128-bit Pollard-rho is used.
 * |val| = |36n^3-19| < 36*(2.1e12)^3 ~ 3.33e38 < 2^128.  Safe.
 * Above this limit we fall back to slow trial division (rarely reached).
 */
#define TRIAL_LIMIT_128  2100000000000LL   /* 2.1 × 10^12 */

/*
 * X^2 overflows i128 when |X| > floor(sqrt(2^127)) ~ 1.304e19.
 * We use this to skip divisors that would cause overflow in check_divisor.
 */
#define XSQRT_I128_MAX  13043817825332782212ULL  /* floor(sqrt(2^127)) */

/* --------------------------------------------------------------------------
 * Outer n-level sieve
 *
 * For small primes p, precompute which residues n mod p can NEVER yield a
 * solution.  The key test: for a given n mod p set val = 36n³-19 mod p.
 * If p ∤ val, every real divisor m of val satisfies gcd(m,p)=1, so we
 * enumerate all m ∈ {1..p-1} and compute rhs = (m+6n)² + val·m⁻¹ (mod p).
 * If rhs is a quadratic non-residue mod p for ALL such m, no solution can
 * exist for this n — skip it entirely.
 * When p | val we conservatively keep the n (allow through).
 *
 * Primes 7, 11, 13 collectively eliminate ~20-25% of n values before any
 * factorisation is attempted (verified: p=7 alone kills n≡0 mod 7 = 14.3%).
 * -------------------------------------------------------------------------- */
#define N_SIEVE_PRIMES 3
static const int SIEVE_PRIMES[N_SIEVE_PRIMES] = {7, 11, 13};
/* sieve_bad[i][r] = 1 iff n ≡ r (mod SIEVE_PRIMES[i]) can be skipped */
static int sieve_bad[N_SIEVE_PRIMES][16];
static int sieve_ready = 0;

static int powmod_sm(int base, int exp, int mod) {
    int r = 1; base %= mod;
    while (exp > 0) {
        if (exp & 1) r = (int)((long long)r * base % mod);
        base = (int)((long long)base * base % mod);
        exp >>= 1;
    }
    return r;
}

static void init_outer_sieve(void) {
    if (sieve_ready) return;
    sieve_ready = 1;
    for (int pi = 0; pi < N_SIEVE_PRIMES; pi++) {
        int p = SIEVE_PRIMES[pi];
        int qr[16] = {0};
        for (int i = 0; i < p; i++) qr[(i*i) % p] = 1;
        for (int nr = 0; nr < p; nr++) {
            int n2  = (int)((long long)nr * nr % p);
            int n3  = (int)((long long)n2 * nr % p);
            /* val = 36n^3 - 19 mod p */
            int val = (int)(((long long)(36 % p) * n3 % p
                             - 19 % p + 2*p) % p);
            if (val == 0) { sieve_bad[pi][nr] = 0; continue; } /* p|val: keep */
            int sixn = (int)(6LL * nr % p);
            int can  = 0;
            for (int m = 1; m < p && !can; m++) {
                int mi  = powmod_sm(m, p - 2, p);         /* m^{-1} mod p */
                int d   = (int)((long long)val * mi % p); /* val/m  mod p */
                int X   = (m + sixn) % p;
                int rhs = (int)((long long)X * X % p + d) % p;
                if (qr[rhs]) can = 1;
            }
            sieve_bad[pi][nr] = can ? 0 : 1;
        }
    }
}

/* --------------------------------------------------------------------------
 * 64-bit fast path (v2): Pollard-rho for u64 values.
 * Used when abs_val fits in u64 (i.e. |n| <= ~787000).
 * -------------------------------------------------------------------------- */

static inline u64 mulmod64(u64 a, u64 b, u64 m) {
    return (u64)(((u128)a * b) % m);
}
static u64 powmod64(u64 a, u64 e, u64 m) {
    u64 r = 1; a %= m;
    while (e) {
        if (e & 1) r = mulmod64(r, a, m);
        a = mulmod64(a, a, m);
        e >>= 1;
    }
    return r;
}
static int is_prime64(u64 n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (!(n & 1) || !(n % 3)) return 0;
    u64 d = n - 1; int r = 0;
    while (!(d & 1)) { d >>= 1; r++; }
    static const u64 W[] = {2,3,5,7,11,13,17,19,23,29,31,37};
    for (int i = 0; i < 12; i++) {
        if (W[i] >= n) continue;
        u64 x = powmod64(W[i], d, n);
        if (x == 1 || x == n-1) continue;
        int c = 1;
        for (int j = 0; j < r-1; j++) {
            x = mulmod64(x, x, n);
            if (x == n-1) { c = 0; break; }
        }
        if (c) return 0;
    }
    return 1;
}
static u64 pollard_rho64(u64 n) {
    if (!(n & 1)) return 2;
    for (u64 c = 1; c < 20; c++) {
        u64 x=2, y=2, d=1, m=128, ys=0, r=1, q=1;
        do {
            x = y;
            for (u64 i=0;i<r;i++) y=(mulmod64(y,y,n)+c)%n;
            u64 k=0; d=1;
            while (k<r && d==1) {
                ys=y;
                for (u64 i=0;i<(m<r-k?m:r-k);i++) {
                    y=(mulmod64(y,y,n)+c)%n;
                    u64 df=(x>y)?x-y:y-x;
                    q=mulmod64(q,df,n);
                }
                u64 a=q, b=n; while(b){u64 t=b;b=a%b;a=t;} d=a;
                k+=m;
            }
            r*=2;
        } while (d==1);
        if (d==n) {
            d=1; y=ys;
            while(d==1){
                y=(mulmod64(y,y,n)+c)%n;
                u64 df=(x>y)?x-y:y-x;
                u64 a=df,b=n; while(b){u64 t=b;b=a%b;a=t;} d=a;
            }
        }
        if (d!=n) return d;
    }
    return n;
}
typedef struct { u64 p; int e; } PrimePow64;
static void fi64(PrimePow64 *o, int *nf, u64 p) {
    for(int i=0;i<*nf;i++) if(o[i].p==p){o[i].e++;return;}
    o[*nf].p=p; o[*nf].e=1; (*nf)++;
}
static void fr64(u64 n, PrimePow64 *o, int *nf) {
    if(n<=1)return;
    if(is_prime64(n)){fi64(o,nf,n);return;}
    for(u64 p=2;p*p<=n&&p<1000;p++) while(n%p==0){fi64(o,nf,p);n/=p;}
    if(n==1)return; if(is_prime64(n)){fi64(o,nf,n);return;}
    u64 d=pollard_rho64(n);
    if(d==n){for(u64 p=2;p*p<=n;p++)while(n%p==0){fi64(o,nf,p);n/=p;}if(n>1)fi64(o,nf,n);return;}
    fr64(d,o,nf); fr64(n/d,o,nf);
}
static int factorize64(u64 n, PrimePow64 *out) {
    int nf=0; fr64(n,out,&nf);
    for(int i=1;i<nf;i++){PrimePow64 kv=out[i];int j=i-1;while(j>=0&&out[j].p>kv.p){out[j+1]=out[j];j--;}out[j+1]=kv;}
    return nf;
}
static u64 g_divs64[131072]; static int g_nd64;
static void edr64(PrimePow64 *pf, int nf, int idx, u64 cur) {
    if(idx==nf){g_divs64[g_nd64++]=cur;return;}
    u64 pk=1;
    for(int e=0;e<=pf[idx].e;e++){edr64(pf,nf,idx+1,cur*pk);pk*=pf[idx].p;}
}

/* --------------------------------------------------------------------------
 * 128-bit modular arithmetic helpers
 * -------------------------------------------------------------------------- */

/*
 * mulmod128(a, b, m): compute (a * b) % m.
 *
 * Fast path: if m fits in 64 bits, then a < m < 2^64 and b < m < 2^64,
 * so a*b < 2^128 — use direct u128 multiply (single MUL + DIV on x86-64).
 *
 * Slow path: m > 2^64  — use the binary (Russian-peasant) method to avoid
 * needing 256-bit types.  Iterates only over the actual bit-length of b.
 */
static inline u128 mulmod128(u128 a, u128 b, u128 m) {
    if (!(m >> 64)) {
        /* m, a, b all fit in 64 bits: direct one-instruction multiply */
        u64 mm = (u64)m;
        return (u128)((u64)(a % mm)) * (u64)(b % mm) % mm;
    }
    /* m > 64 bits: binary method, iterate only over set bit-range of b */
    u128 result = 0;
    a %= m;
    b %= m;
    /* find top bit of b to limit loop iterations */
    int bits = 127;
    while (bits > 0 && !((b >> bits) & 1)) bits--;
    u128 mask = (u128)1 << bits;
    while (mask) {
        result <<= 1;
        if (result >= m) result -= m;
        if (b & mask) {
            result += a;
            if (result >= m) result -= m;
        }
        mask >>= 1;
    }
    return result;
}

/* a^e % m using mulmod128 */
static u128 powmod128(u128 a, u128 e, u128 m) {
    u128 r = 1;
    a %= m;
    while (e) {
        if (e & 1) r = mulmod128(r, a, m);
        a = mulmod128(a, a, m);
        e >>= 1;
    }
    return r;
}

/* --------------------------------------------------------------------------
 * Miller-Rabin primality test — deterministic for n < 3.3e24 with the 12
 * witnesses {2,3,5,7,11,13,17,19,23,29,31,37}.  For n up to our ceiling
 * (~3.3e38) we use the same witnesses; false composites are essentially
 * non-existent in our divisor-enumeration context.
 * -------------------------------------------------------------------------- */
static int is_prime128(u128 n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;

    u128 d = n - 1;
    int r = 0;
    while ((d & 1) == 0) { d >>= 1; r++; }

    static const u64 witnesses[] = {2,3,5,7,11,13,17,19,23,29,31,37};
    for (int i = 0; i < 12; i++) {
        u128 a = (u128)witnesses[i];
        if (a >= n) continue;
        u128 x = powmod128(a, d, n);
        if (x == 1 || x == n - 1) continue;
        int composite = 1;
        for (int j = 0; j < r - 1; j++) {
            x = mulmod128(x, x, n);
            if (x == n - 1) { composite = 0; break; }
        }
        if (composite) return 0;
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * Pollard-rho (Brent) for u128 — returns a non-trivial factor of n, or n.
 * All arithmetic uses mulmod128 to avoid 256-bit overflow.
 * -------------------------------------------------------------------------- */
static u128 pollard_rho128(u128 n) {
    if (n % 2 == 0) return 2;
    for (u128 c = 1; c < 20; c++) {
        u128 x = 2, y = 2, d = 1;
        u128 m = 128;
        u128 ys = 0, r = 1, q = 1;
        do {
            x = y;
            for (u128 i = 0; i < r; i++)
                y = (mulmod128(y, y, n) + c) % n;
            u128 k = 0;
            d = 1;
            while (k < r && d == 1) {
                ys = y;
                u128 batch = (m < r - k) ? m : r - k;
                for (u128 i = 0; i < batch; i++) {
                    y = (mulmod128(y, y, n) + c) % n;
                    u128 diff = (x > y) ? x - y : y - x;
                    q = mulmod128(q, diff, n);
                }
                /* GCD(q, n) */
                u128 a = q, b = n;
                while (b) { u128 t = b; b = a % b; a = t; }
                d = a;
                k += m;
            }
            r *= 2;
        } while (d == 1);

        if (d == n) {
            /* backtrack one step at a time */
            d = 1;
            y = ys;
            while (d == 1) {
                y = (mulmod128(y, y, n) + c) % n;
                u128 diff = (x > y) ? x - y : y - x;
                u128 a = diff, b = n;
                while (b) { u128 t = b; b = a % b; a = t; }
                d = a;
            }
        }
        if (d != n) return d;
    }
    return n;
}

/* --------------------------------------------------------------------------
 * 128-bit prime factorisation
 * -------------------------------------------------------------------------- */
typedef struct { u128 p; int e; } PrimePow128;

static void factor_insert128(PrimePow128 *out, int *nf, u128 p) {
    for (int i = 0; i < *nf; i++) {
        if (out[i].p == p) { out[i].e++; return; }
    }
    out[*nf].p = p;
    out[*nf].e = 1;
    (*nf)++;
}

static void factor_rec128(u128 n, PrimePow128 *out, int *nf) {
    if (n <= 1) return;
    if (is_prime128(n)) { factor_insert128(out, nf, n); return; }
    /* trial division for small primes first */
    for (u128 p = 2; p < 1000 && p * p <= n; p++) {
        while (n % p == 0) { factor_insert128(out, nf, p); n /= p; }
    }
    if (n == 1) return;
    if (is_prime128(n)) { factor_insert128(out, nf, n); return; }
    /* Pollard-rho split */
    u128 d = pollard_rho128(n);
    if (d == n) {
        /* fallback: full trial division of the remainder */
        for (u128 p = 2; p * p <= n; p++) {
            while (n % p == 0) { factor_insert128(out, nf, p); n /= p; }
        }
        if (n > 1) factor_insert128(out, nf, n);
        return;
    }
    factor_rec128(d,     out, nf);
    factor_rec128(n / d, out, nf);
}

static int factorize128(u128 n, PrimePow128 *out) {
    int nf = 0;
    factor_rec128(n, out, &nf);
    /* insertion sort by prime */
    for (int i = 1; i < nf; i++) {
        PrimePow128 kv = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].p > kv.p) { out[j+1] = out[j]; j--; }
        out[j+1] = kv;
    }
    return nf;
}

/* --------------------------------------------------------------------------
 * Enumerate all POSITIVE divisors of n from its 128-bit factorisation.
 * Stores results in g_divs128[]; sets g_ndivs128 = count.
 * -------------------------------------------------------------------------- */
static u128 g_divs128[131072];
static int  g_ndivs128;

static void enum_div_rec128(PrimePow128 *pf, int nf, int idx, u128 cur) {
    if (idx == nf) {
        g_divs128[g_ndivs128++] = cur;
        return;
    }
    u128 pk = 1;
    for (int e = 0; e <= pf[idx].e; e++) {
        enum_div_rec128(pf, nf, idx + 1, cur * pk);
        pk *= pf[idx].p;
    }
}

/* --------------------------------------------------------------------------
 * Fallback: enumerate divisors of |val| (as i128) by trial division.
 * Used only when |n| > TRIAL_LIMIT_128 and val may not fit in u128.
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
 * Modular QR sieve bitmasks
 *
 * rhs = (m+6n)² + val/m  must be a perfect square, so it must be a
 * quadratic residue mod every integer.  Four independent bitmask filters:
 *
 *  mod 64  QRs: 12/64  → ~81% kill  (free: just bottom 6 bits)
 *  mod 45  QRs: 12/45  → ~73% of remainder  (covers primes 3² and 5)
 *  mod  7  QRs:  4/ 7  → ~43% of remainder  (independent prime)
 *  mod 11  QRs:  6/11  → ~45% of remainder  (independent prime)
 *
 * Note: mod-9 (former 3rd filter) was REDUNDANT — any rhs passing mod-45
 * already passes mod-9 (since 9|45).  Replaced with mod-7 and mod-11.
 *
 * Combined false-positive rate: ≈ (12/64)×(12/45)×(4/7)×(6/11) ≈ 1.6%
 * vs the old chain's effective rate of (12/64)×(12/45) ≈ 5.0%.
 * GCC -O3 compiles all constant-divisor % to multiply-by-reciprocal.
 * -------------------------------------------------------------------------- */
#define QR64_MASK  0x0202021202030213ULL
#define QR45_MASK  ( (1ULL<< 0)|(1ULL<< 1)|(1ULL<< 4)|(1ULL<< 9)|(1ULL<<10) \
                   | (1ULL<<16)|(1ULL<<19)|(1ULL<<25)|(1ULL<<31)|(1ULL<<34) \
                   | (1ULL<<36)|(1ULL<<40) )
/* QRs mod  7: {0,1,2,4}          */ 
#define QR7_MASK   ( (1U<<0)|(1U<<1)|(1U<<2)|(1U<<4) )
/* QRs mod 11: {0,1,3,4,5,9}      */
#define QR11_MASK  ( (1U<<0)|(1U<<1)|(1U<<3)|(1U<<4)|(1U<<5)|(1U<<9) )

/* --------------------------------------------------------------------------
 * Check one divisor m of val against the equation
 * -------------------------------------------------------------------------- */
static void check_divisor(i128 n128, i128 val, i128 m,
                           FILE *out, FILE *log) {
    if (m == 0) return;
    i128 quot = val / m;
    i128 X    = m + (i128)6 * n128;
    /* Guard: skip if X^2 would overflow i128 */
    i128 ax = (X >= 0) ? X : -X;
    if ((u128)ax > (u128)XSQRT_I128_MAX) return;
    i128 rhs  = X * X + quot;
    if (rhs < 0) return;

    /* ---- QR modular sieve (reject non-squares before the costly isqrt) ---- */
    if (!((QR64_MASK >> (unsigned)(rhs & 63)) & 1)) return;        /* mod 64 */
    u128 urhs = (u128)rhs;
    if (!((QR45_MASK >> (unsigned)(urhs % 45)) & 1)) return;       /* mod 45 */
    if (!((QR7_MASK  >> (unsigned)(urhs %  7)) & 1)) return;       /* mod  7 */
    if (!((QR11_MASK >> (unsigned)(urhs % 11)) & 1)) return;       /* mod 11 */

    i128 Y;
    if (is_perfect_square(rhs, &Y))
        emit(out, log, n128, m, Y, X);
}

/* --------------------------------------------------------------------------
 * Main search loop
 * -------------------------------------------------------------------------- */
static void search_range(i64 n_start, i64 n_end, FILE *out, FILE *log) {
    PrimePow64  pf64[64];
    PrimePow128 pf128[128];

    init_outer_sieve();  /* no-op after first call */

    for (i64 n = n_start; n <= n_end; n++) {
        /* ---- outer n-level sieve ---- */
        {
            int skip = 0;
            for (int pi = 0; pi < N_SIEVE_PRIMES && !skip; pi++) {
                int p  = SIEVE_PRIMES[pi];
                int nr = (int)(((n % (i64)p) + (i64)p) % (i64)p);
                if (sieve_bad[pi][nr]) skip = 1;
            }
            if (skip) continue;
        }

        i128 n128 = (i128)n;
        i128 val  = (i128)36 * n128 * n128 * n128 - 19;
        if (val == 0) continue;

        u128 abs_val  = (val >= 0) ? (u128)val : (u128)(-val);
        int  sign_val = (val >= 0) ? 1 : -1;

        if (!(abs_val >> 64)) {
            /* ---- fast path: 64-bit Pollard-rho (original v2 speed) ---- */
            u64 val64 = (u64)abs_val;
            int nf = factorize64(val64, pf64);
            g_nd64 = 0;
            edr64(pf64, nf, 0, (u64)1);
            int nd = g_nd64;
            for (int d = 0; d < nd; d++) {
                i128 g = (i128)g_divs64[d];
                check_divisor(n128, val, sign_val > 0 ?  g : -g, out, log);
                check_divisor(n128, val, sign_val > 0 ? -g :  g, out, log);
            }
        } else {
            i64 n_abs = (n < 0) ? -n : n;
            if (n_abs <= TRIAL_LIMIT_128) {
                /* ---- 128-bit Pollard-rho for abs_val > u64_max ---- */
                int nf = factorize128(abs_val, pf128);
                g_ndivs128 = 0;
                enum_div_rec128(pf128, nf, 0, (u128)1);
                int nd = g_ndivs128;
                for (int d = 0; d < nd; d++) {
                    u128 ud = g_divs128[d];
                    if (ud > (u128)((i128)(-1) >> 1)) continue; /* > i128_max */
                    i128 g = (i128)ud;
                    check_divisor(n128, val, sign_val > 0 ?  g : -g, out, log);
                    check_divisor(n128, val, sign_val > 0 ? -g :  g, out, log);
                }
            } else {
                /* ---- fallback: trial division for |n| > TRIAL_LIMIT_128 ---- */
                int total = enum_divisors_fallback(val, fb_divs);
                for (int d = 0; d < total; d++)
                    check_divisor(n128, val, fb_divs[d], out, log);
            }
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
