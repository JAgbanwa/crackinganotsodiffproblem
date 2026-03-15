/*
 * worker.c  --  Charity Engine / BOINC worker  (v4: rational D-denominator)
 *
 * Searches for integer Y satisfying:
 *
 *   D = 1 (integer mode, default):
 *     Y^2 = (m + 6n)^2 + (36n^3 - 19) / m       m != 0, m | 36n^3-19
 *
 *   D > 1 (rational mode — new):
 *     n = N/D,  m = M/D^3     (N,M integers, M | 36N^3-19D^3)
 *     Y^2 = Xd^2 + (36N^3-19D^3)/M   where Xd = (M + 6*N*D^2)/D^3
 *     (Xd must be integer, i.e. D^3 | M + 6*N*D^2)
 *     Solutions give x^3 + y^3 + z^3 = 114 via rational triples.
 *
 * wu_file format:  "N_start N_end"        (D=1 default, backward compat)
 *                  "N_start N_end D"       (D > 1 for rational search)
 *
 * Output:  n=N/D m=M/D^3 Y=Y X=Xd  (D=1 omits denominators for clarity)
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

static int powmod_sm(int base, int exp, int mod) {
    int r = 1; base %= mod;
    while (exp > 0) {
        if (exp & 1) r = (int)((long long)r * base % mod);
        base = (int)((long long)base * base % mod);
        exp >>= 1;
    }
    return r;
}

/*
 * init_outer_sieve_D: for a given denominator D, precompute which residues
 * N mod p (for p in SIEVE_PRIMES) can never yield an integer Y solution.
 * Formula: val_D = 36*N^3 - 19*D^3 mod p.
 * When p | val_D (val=0): conservatively allow N through.
 * Otherwise: for every unit m mod p, compute Xd = (m+6*N*D^2)*inv(D^3) mod p,
 * rhs = Xd^2 + val_D*inv(m) mod p; if rhs is never a QR mod p for any m,
 * mark this N residue as bad (skip).
 * For D=1 this reduces to the old formula identically.
 */
static void init_outer_sieve_D(int D) {
    for (int pi = 0; pi < N_SIEVE_PRIMES; pi++) {
        int p    = SIEVE_PRIMES[pi];
        int qr[16] = {0};
        for (int i = 0; i < p; i++) qr[(i*i) % p] = 1;
        int D2p   = powmod_sm(D % p, 2, p);          /* D^2 mod p */
        int D3p   = powmod_sm(D % p, 3, p);          /* D^3 mod p */
        int invD3 = (D3p == 0) ? 1 : powmod_sm(D3p, p - 2, p); /* D^-3 mod p */
        /* Note: for p in {7,11,13} and D in {1..6}, D3p != 0 always. */
        for (int nr = 0; nr < p; nr++) {
            int n2  = (int)((long long)nr * nr % p);
            int n3  = (int)((long long)n2 * nr % p);
            /* val_D = 36*N^3 - 19*D^3  mod p */
            int val = (int)(((long long)(36 % p) * n3
                             - (long long)(19 % p) * D3p % p + 3LL*p) % p);
            if (val == 0) { sieve_bad[pi][nr] = 0; continue; }
            int can = 0;
            for (int m = 1; m < p && !can; m++) {
                int mi     = powmod_sm(m, p - 2, p);
                int d      = (int)((long long)val * mi % p);    /* val_D/M mod p */
                int X_int  = (int)(((long long)m
                              + 6LL * nr % p * D2p) % p);       /* M+6*N*D^2 mod p */
                int Xd     = (int)((long long)X_int * invD3 % p); /* /D^3 mod p */
                int rhs    = (int)((long long)Xd * Xd % p + d) % p;
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
 * emit_D: print one solution.  D=1 → integer n,m (same format as before).
 * D>1 → rational n=N/D, m=M/D^3.
 * -------------------------------------------------------------------------- */
static void print_nm(FILE *f, i128 N, i128 M, i128 D3, int D_val) {
    fprintf(f, "n="); print128(f, N);
    if (D_val != 1) fprintf(f, "/%d", D_val);
    fprintf(f, " m="); print128(f, M);
    if (D_val != 1) { fprintf(f, "/"); print128(f, D3); }
}
static void emit_D(FILE *out, FILE *log,
                   i128 N128, i128 M, i128 Y, i128 Xd,
                   i128 D3, int D_val) {
    print_nm(out, N128, M, D3, D_val);
    fprintf(out, " Y="); print128(out, Y);
    fprintf(out, " X="); print128(out, Xd);
    fprintf(out, "\n"); fflush(out);
    if (Y != 0) {
        print_nm(out, N128, M, D3, D_val);
        fprintf(out, " Y="); print128(out, -Y);
        fprintf(out, " X="); print128(out, Xd);
        fprintf(out, "\n"); fflush(out);
    }
    if (log) {
        fprintf(log, "[SOLUTION] ");
        print_nm(log, N128, M, D3, D_val);
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
 * check_divisor_D: unified integer+rational divisor check.
 *
 * For D=1: identical to the old check_divisor (N128=n, val_D=val, M=m,
 *           D2=1, D3=1 — so X_int = M+6*N, divisibility trivially holds,
 *           Xd = X_int, rhs = Xd^2 + val/M).
 * For D>1: n=N/D, m=M/D^3; Xd=(M+6*N*D^2)/D^3 must be integer.
 * -------------------------------------------------------------------------- */
static void check_divisor_D(i128 N128, i128 valD, i128 M,
                              i128 D2,   i128 D3,  int D_val,
                              FILE *out, FILE *log) {
    if (M == 0) return;
    i128 quot  = valD / M;                    /* integer since M | valD */
    i128 X_int = M + (i128)6 * N128 * D2;    /* = D^3 * (m + 6n) */
    /* For D>1: must have D^3 | X_int for rhs to be integer */
    if (D3 != 1 && X_int % D3 != 0) return;
    i128 Xd = (D3 == 1) ? X_int : X_int / D3;
    /* overflow guard: skip if Xd^2 would exceed i128 */
    i128 axd = (Xd >= 0) ? Xd : -Xd;
    if ((u128)axd > (u128)XSQRT_I128_MAX) return;
    i128 rhs = Xd * Xd + quot;
    if (rhs < 0) return;

    /* ---- QR modular sieve ---- */
    if (!((QR64_MASK >> (unsigned)(rhs & 63)) & 1)) return;        /* mod 64 */
    u128 urhs = (u128)rhs;
    if (!((QR45_MASK >> (unsigned)(urhs % 45)) & 1)) return;       /* mod 45 */
    if (!((QR7_MASK  >> (unsigned)(urhs %  7)) & 1)) return;       /* mod  7 */
    if (!((QR11_MASK >> (unsigned)(urhs % 11)) & 1)) return;       /* mod 11 */

    i128 Y;
    if (is_perfect_square(rhs, &Y))
        emit_D(out, log, N128, M, Y, Xd, D3, D_val);
}

/* --------------------------------------------------------------------------
 * Main search loop
 * -------------------------------------------------------------------------- */
static void search_range_D(i64 n_start, i64 n_end, int D, FILE *out, FILE *log) {
    PrimePow64  pf64[64];
    PrimePow128 pf128[128];

    init_outer_sieve_D(D);   /* (re)build sieve for this D */

    /* Precompute D powers as i128 so arithmetic is uniform */
    i128 D_128 = (i128)D;
    i128 D2    = D_128 * D_128;          /* D^2 */
    i128 D3    = D2    * D_128;          /* D^3 */
    i128 D3_19 = D3    * (i128)19;       /* 19 * D^3  (constant term in valD) */

    for (i64 N = n_start; N <= n_end; N++) {
        /* ---- outer N-level sieve ---- */
        {
            int skip = 0;
            for (int pi = 0; pi < N_SIEVE_PRIMES && !skip; pi++) {
                int p  = SIEVE_PRIMES[pi];
                int nr = (int)(((N % (i64)p) + (i64)p) % (i64)p);
                if (sieve_bad[pi][nr]) skip = 1;
            }
            if (skip) continue;
        }

        i128 N128  = (i128)N;
        i128 valD  = (i128)36 * N128 * N128 * N128 - D3_19;  /* 36N^3 - 19D^3 */
        if (valD == 0) continue;

        u128 abs_val  = (valD >= 0) ? (u128)valD : (u128)(-valD);
        int  sign_val = (valD >= 0) ? 1 : -1;

        if (!(abs_val >> 64)) {
            u64 val64 = (u64)abs_val;
            int nf = factorize64(val64, pf64);
            g_nd64 = 0;
            edr64(pf64, nf, 0, (u64)1);
            int nd = g_nd64;
            for (int d = 0; d < nd; d++) {
                i128 g = (i128)g_divs64[d];
                check_divisor_D(N128, valD, sign_val > 0 ?  g : -g,
                                D2, D3, D, out, log);
                check_divisor_D(N128, valD, sign_val > 0 ? -g :  g,
                                D2, D3, D, out, log);
            }
        } else {
            i64 n_abs = (N < 0) ? -N : N;
            if (n_abs <= TRIAL_LIMIT_128) {
                int nf = factorize128(abs_val, pf128);
                g_ndivs128 = 0;
                enum_div_rec128(pf128, nf, 0, (u128)1);
                int nd = g_ndivs128;
                for (int d = 0; d < nd; d++) {
                    u128 ud = g_divs128[d];
                    if (ud > (u128)((i128)(-1) >> 1)) continue;
                    i128 g = (i128)ud;
                    check_divisor_D(N128, valD, sign_val > 0 ?  g : -g,
                                    D2, D3, D, out, log);
                    check_divisor_D(N128, valD, sign_val > 0 ? -g :  g,
                                    D2, D3, D, out, log);
                }
            } else {
                int total = enum_divisors_fallback(valD, fb_divs);
                for (int d = 0; d < total; d++)
                    check_divisor_D(N128, valD, fb_divs[d], D2, D3, D, out, log);
            }
        }

        if ((N % 10000) == 0 && log) {
            fprintf(log, "[progress] N=%lld D=%d\n", (long long)N, D);
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
    int D = 1;   /* default: integer search */
    int nread = fscanf(wu, "%" SCNd64 " %" SCNd64 " %d",
                       &n_start, &n_end, &D);
    if (nread < 2) {
        fprintf(stderr, "Bad wu_file format (expect: N_start N_end [D])\n");
        fclose(wu); return 1;
    }
    fclose(wu);
    if (D < 1 || D > 1000) {
        fprintf(stderr, "D=%d out of range [1,1000]\n", D);
        return 1;
    }

    FILE *out = fopen(argv[2], "w");
    if (!out) { perror("open output_file"); return 1; }
    FILE *log = fopen("worker_progress.log", "a");

    if (log) {
        fprintf(log, "[start] N=[%lld, %lld] D=%d\n",
                (long long)n_start, (long long)n_end, D);
        fflush(log);
    }

    search_range_D(n_start, n_end, D, out, log);

    if (log) {
        fprintf(log, "[done] N=[%lld, %lld] D=%d\n",
                (long long)n_start, (long long)n_end, D);
        fclose(log);
    }
    fclose(out);
    return 0;
}
