/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O3")                 /* -Os makes this ~40 % slower on the ARM7 */
#endif
#ifndef C2C_STATS
#define C2C_STATS 0                        /* per-bin counting off on the iPod */
#endif
#ifndef C2C_MAX_NAL
#define C2C_MAX_NAL (192 * 1024)
#endif
/* bcm overlay v123: H.264 CABAC -> CAVLC entropy transcoder.  See bcm_cab2cav.h.
 * Clause numbers refer to ITU-T H.264 (Rec. H.264 | ISO/IEC 14496-10). */
#include <string.h>
#include "bcm_cab2cav.h"
#include "bcm_cab2cav_tables.h"

#ifndef C2C_MAX_NAL
#define C2C_MAX_NAL (256 * 1024)              /* largest output NAL unit */
#endif

#ifndef C2C_DBG
#define C2C_DBG(...) ((void)0)
#endif
#ifndef C2C_STATS
#define C2C_STATS 1
#endif
#if C2C_STATS
#define BIN() (c->st.bins++)
#else
#define BIN() ((void)0)
#endif

/* ------------------------------------------------------------------ bit reader (RBSP) */
typedef struct { const uint8_t *buf; uint32_t pos, end; } br_t;   /* pos / end in bits */

static inline uint32_t br_u(br_t *b, int n)          /* 1 <= n <= 25 */
{
    const uint8_t *p = b->buf + (b->pos >> 3);
    uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    v <<= (b->pos & 7);
    b->pos += n;
    return v >> (32 - n);
}
static inline uint32_t br_u1(br_t *b)
{
    uint32_t v = (b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}
static uint32_t br_ue(br_t *b)
{
    int lz = 0;
    while (!br_u1(b)) { if (++lz > 31 || b->pos > b->end) return 0xffffffff; }
    if (!lz) return 0;
    if (lz > 25) { uint32_t hi = br_u(b, lz - 16); return ((1u << lz) - 1) + ((hi << 16) | br_u(b, 16)); }
    return ((1u << lz) - 1) + br_u(b, lz);
}
static int32_t br_se(br_t *b)
{
    uint32_t k = br_ue(b);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

/* ------------------------------------------------------------------ bit writer (RBSP; escaped afterwards) */
typedef struct { uint8_t *p, *end; uint32_t acc; int left; int err; int big; uint32_t clamped, escaped; } bw_t;

static inline void bw_word(bw_t *w, uint32_t a)
{
    if (w->p + 4 > w->end) { w->err = 1; return; }
    w->p[0] = (uint8_t)(a >> 24); w->p[1] = (uint8_t)(a >> 16); w->p[2] = (uint8_t)(a >> 8); w->p[3] = (uint8_t)a;
    w->p += 4;
}
static inline void bw_put(bw_t *w, uint32_t v, int n)  /* v < 2^n, n <= 24 */
{
    if (n < w->left) { w->acc = (w->acc << n) | v; w->left -= n; return; }
    n -= w->left;
    bw_word(w, (w->acc << w->left) | (v >> n));
    w->acc = v;
    w->left = 32 - n;
}
static uint8_t log2tab[256];
static inline void bw_ue(bw_t *w, uint32_t v)
{
    uint32_t x = v + 1; int len;
    if (x < 256) { len = log2tab[x]; bw_put(w, x, 2 * len + 1); return; }
    len = 8;
    while ((x >> len) > 1) len++;                       /* len = floor(log2(x)) */
    if (len > 12) { bw_put(w, 0, len); bw_put(w, x, len + 1); return; }
    bw_put(w, x, 2 * len + 1);
}
static inline void bw_se(bw_t *w, int32_t v) { bw_ue(w, v > 0 ? 2u * v - 1 : (uint32_t)(-2 * v)); }
static void bw_trailing(bw_t *w)                        /* rbsp_trailing_bits, then flush */
{
    bw_put(w, 1, 1);
    int used = 32 - w->left;
    if (used & 7) bw_put(w, 0, 8 - (used & 7));
    used = 32 - w->left;
    for (int sh = used - 8; sh >= 0; sh -= 8) {
        if (w->p >= w->end) { w->err = 1; return; }
        *w->p++ = (uint8_t)(w->acc >> sh);
    }
    w->left = 32;
}
static void bw_copy(bw_t *w, br_t *b, uint32_t from, uint32_t to)
{
    b->pos = from;
    while (to - b->pos >= 16) bw_put(w, br_u(b, 16), 16);
    if (to > b->pos) { int n = to - b->pos; bw_put(w, br_u(b, n), n); }
}
/* RBSP -> NAL payload (emulation_prevention_three_byte); returns the length or -1 */
static int escape(const uint8_t *in, int n, uint8_t *out, int cap)
{
    int o = 0, z = 0;
    for (int i = 0; i < n; i++) {
        uint32_t b = in[i];
        if (z >= 2 && b <= 3) { if (o >= cap) return -1; out[o++] = 3; z = 0; }
        if (o >= cap) return -1;
        out[o++] = (uint8_t)b;
        z = b ? 0 : z + 1;
    }
    return o;
}

/* ------------------------------------------------------------------ parameter sets */
typedef struct {
    uint8_t valid, profile, log2_max_frame_num, poc_type, log2_max_poc_lsb, delta_pic_order_always_zero;
    uint8_t frame_mbs_only;
    uint16_t mb_w, mb_h;
} sps_t;
typedef struct {
    uint8_t valid, sps_id, cabac, bottom_field_pic_order, weighted_pred, deblock_ctrl, redundant_cnt;
    uint8_t num_ref_l0, num_ref_l1;
    int8_t init_qp;
} pps_t;

/* arithmetic decoder (9.3.1.2, 9.3.3.2).  val = codIOffset * 2^k + the next k bits of the
   stream, so a decision compares val with codIRange << k and renormalisation only moves k. */
typedef struct { uint32_t range, val; int k; const uint8_t *p; } eng_t;

/* ------------------------------------------------------------------ per-macroblock neighbour data */
/* Only what the macroblocks to the right and below need: the right column and the bottom row of
   each per-4x4 quantity, already reduced by the neighbour rules (I_PCM, P_Skip, coded_block_pattern). */
enum { T_SKIP = 1, T_P, T_I4, T_I16, T_PCM };
/* edge words: byte i = row i of the right column (_r) or column i of the bottom row (_b) */
typedef struct {
    uint32_t slice;            /* slice serial number: neighbours must share it */
    uint8_t type, cbp, cmode, dc;              /* dc: coded_block_flag bit0 luma DC, bit1 Cb DC, bit2 Cr DC */
    uint32_t cbf_r, cbf_b;     /* luma coded_block_flag (0 / 1), neighbour rules applied */
    uint32_t nz_r, nz_b;       /* luma TotalCoeff */
    uint32_t mvx_r, mvx_b, mvy_r, mvy_b;       /* |mvd|, clipped to 64; 0 unless P */
    uint32_t refp_r, refp_b;   /* ref_idx_l0 > 0; 0 unless P */
    uint32_t ccbf_r, ccbf_b;   /* chroma AC: byte comp * 2 + i */
    uint32_t cnz_r, cnz_b;
} mbi_t;

struct c2c {
    /* arithmetic decoder first: small offsets in the hot loops */
    eng_t e;
    br_t *b;                   /* RBSP of the slice being transcoded */
    uint8_t ctx[460];
    c2c_stats_t st;
    uint32_t slice_serial;
    int big;                   /* code levels beyond CAVLC Main with level_prefix 16 */
    sps_t sps[32];
    pps_t pps[256];
    mbi_t mb[C2C_MAX_MBS];
    uint32_t mbd[(sizeof(int) * 40 + 32 + 16 * 4 + 27 * 50 + 384 + 64) / 4 + 64];   /* one decoded macroblock (mbd_t) */
    uint8_t obuf[C2C_MAX_NAL];                 /* CAVLC RBSP before escaping */
};

static const uint8_t trans_lps[64] = {
    0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9,11,11,12,13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
   24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63 };
static uint8_t renorm_shift[256];
static void eng_tables(void);

uint32_t c2c_state_size(void) { return sizeof(struct c2c); }

c2c_t *c2c_init(void *mem, uint32_t size)
{
    if (size < sizeof(struct c2c)) return 0;
    memset(mem, 0, sizeof(struct c2c));
    for (int r = 1; r < 256; r++) { int n = 0; while ((r << n) < 256) n++; renorm_shift[r] = n; }
    for (int x = 1; x < 256; x++) { int l = 0; while ((x >> l) > 1) l++; log2tab[x] = (uint8_t)l; }
    eng_tables();
    return (c2c_t *)mem;
}
const c2c_stats_t *c2c_stats(c2c_t *c) { return &c->st; }
void c2c_set_escape(c2c_t *c, int on) { c->big = on; }
const char *c2c_errstr(int e)
{
    switch (e) {
    case C2C_ERR_SYNTAX: return "bitstream error";
    case C2C_ERR_UNSUPPORTED: return "unsupported tool (B/SP/SI slice, interlace, weighted prediction, High profile)";
    case C2C_ERR_SPACE: return "output buffer too small";
    case C2C_ERR_LEVEL: return "coefficient too large for CAVLC Main";
    case C2C_ERR_NOPARAM: return "missing SPS/PPS";
    }
    return "ok";
}

/* ------------------------------------------------------------------ CABAC engine (9.3.1.2, 9.3.3.2) */
/* tables indexed by the context byte (pStateIdx << 1 | valMPS) */
static uint8_t rlps[128][4], next_mps[128], next_lps[128];

static void eng_tables(void)
{
    for (int s = 0; s < 128; s++) {
        int st = s >> 1, m = s & 1;
        for (int q = 0; q < 4; q++) rlps[s][q] = range_lps[st][q];
        next_mps[s] = (uint8_t)(st < 62 ? s + 2 : s);
        next_lps[s] = (uint8_t)((trans_lps[st] << 1) | (st == 0 ? m ^ 1 : m));
    }
}
#define ENG_REFILL(e) do { if ((e)->k < 8) { (e)->val = ((e)->val << 16) | ((uint32_t)(e)->p[0] << 8) | (e)->p[1]; \
                                            (e)->p += 2; (e)->k += 16; } } while (0)

static inline int e_dec(eng_t *e, uint8_t *ctx, int ci)
{
    uint32_t s = ctx[ci];
    uint32_t lps = rlps[s][(e->range >> 6) & 3];
    uint32_t range = e->range - lps, t = range << e->k;
    if (e->val < t) {
        ctx[ci] = next_mps[s];
        if (range < 256) { range <<= 1; e->k--; ENG_REFILL(e); }
        e->range = range;
        return s & 1;
    }
    e->val -= t;
    ctx[ci] = next_lps[s];
    int n = renorm_shift[lps];
    e->range = lps << n;
    e->k -= n;
    ENG_REFILL(e);
    return (s & 1) ^ 1;
}
static inline int e_bypass(eng_t *e)
{
    e->k--;
    uint32_t t = e->range << e->k;
    int bin = 0;
    if (e->val >= t) { e->val -= t; bin = 1; }
    ENG_REFILL(e);
    return bin;
}
/* the decoding functions below use E (engine) and c->ctx; E points to a local copy of the
   engine so that the byte stores into the context table do not force it back to memory */
#define AI __attribute__((always_inline)) inline
#if C2C_STATS
#define cab_dec(c, ci) (c->st.bins++, e_dec(E, (c)->ctx, (ci)))
#define cab_bypass(c) (c->st.bins++, e_bypass(E))
#else
#define cab_dec(c, ci) e_dec(E, (c)->ctx, (ci))
#define cab_bypass(c) e_bypass(E)
#endif
static AI int cab_term(c2c_t *c, eng_t *E)
{
    (void)c;
    eng_t *e = E;
    BIN();
    e->range -= 2;
    if (e->val >= (e->range << e->k)) return 1;           /* no renormalisation */
    if (e->range < 256) {
        int n = renorm_shift[e->range];
        e->range <<= n; e->k -= n;
        ENG_REFILL(e);
    }
    return 0;
}
/* start the engine at byte-aligned bit position pos of c->b */
static void cab_init_engine(c2c_t *c, eng_t *E, uint32_t pos)
{
    const uint8_t *p = c->b->buf + (pos >> 3);
    E->val = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    E->k = 15;
    E->p = p + 3;
    E->range = 510;
}
static inline uint32_t cab_pos(c2c_t *c, eng_t *E) { return (uint32_t)(E->p - c->b->buf) * 8 - E->k; }

static void cab_init_ctx(c2c_t *c, int slice_qp, int table)
{
    int qp = slice_qp < 0 ? 0 : slice_qp > 51 ? 51 : slice_qp;
    for (int i = 0; i < 460; i++) {
        int pre = ((ctx_init[table][i][0] * qp) >> 4) + ctx_init[table][i][1];
        pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
        c->ctx[i] = pre <= 63 ? (uint8_t)((63 - pre) << 1) : (uint8_t)(((pre - 64) << 1) | 1);
    }
}

/* ------------------------------------------------------------------ helpers */
static inline int r2b8(int r) { return ((r >> 3) << 1) | ((r & 3) >> 1); }   /* raster 4x4 -> 8x8 index */

/* per-macroblock context caches: luma 8 bytes per row (row / column -1 = neighbours, the 4x4
   interior of each row is one aligned word), chroma 4 bytes per row, both components */
#define I8(x, y) (((y) + 1) * 8 + (x) + 4)
#define I4(x, y) (((y) + 1) * 4 + (x) + 1)
typedef struct {
    int avA, avB, intra;
    mbi_t *A, *B, *cur;
    uint32_t cbf_w[10], nz_w[10], mvx_w[10], mvy_w[10], refp_w[10];
    uint8_t ccbf[2][12];
    int8_t cnz[2][12];
} mc_t;
#define B8(a) ((uint8_t *)(a))
static const uint8_t blkx[16] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3 };
static const uint8_t blky[16] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };

/* ------------------------------------------------------------------ CABAC syntax elements */
static AI int cab_ue_k(c2c_t *c, eng_t *E, int k)                     /* Exp-Golomb bypass suffix (UEGk) */
{
    (void)c;
    uint32_t v = 0;
    while (cab_bypass(c)) { v += 1u << k; if (++k > 30) return -1; }
    while (k--) v += (uint32_t)cab_bypass(c) << k;
    return (int)v;
}

static const uint8_t sig_off[5] = { 0, 15, 29, 44, 47 }, abs_off[5] = { 0, 10, 20, 30, 39 };

/* one residual block: its non-zero coefficients from the highest scan position down
   (the order CABAC decodes the levels in and the order CAVLC writes them in) */
typedef struct { uint8_t n; uint8_t pos[16]; int16_t lev[16]; } blk_t;

/* residual_block_cabac after coded_block_flag == 1: fills k, returns the coefficient count.
   Most bins are decoded here, so the engine is kept in local variables. */
static int cab_block(c2c_t *c, eng_t *E, int cat, int maxn, blk_t *k)
{
    uint8_t sig[16];
    int n = 0;
    eng_t e = *E;
    uint8_t *ctx = c->ctx;
    uint8_t *sctx = ctx + 105 + sig_off[cat], *lctx = ctx + 166 + sig_off[cat];
#if C2C_STATS
    uint32_t bins = 0;
#define LBIN() bins++
#else
#define LBIN() ((void)0)
#endif
    if (cat != 3) {
        for (int i = 0; i < maxn - 1; i++) {
            LBIN();
            if (e_dec(&e, sctx, i)) {
                sig[n++] = (uint8_t)i;
                LBIN();
                if (e_dec(&e, lctx, i)) goto levels;
            }
        }
    } else {
        for (int i = 0; i < 3; i++) {
            int inc = i < 2 ? i : 2;
            LBIN();
            if (e_dec(&e, sctx, inc)) {
                sig[n++] = (uint8_t)i;
                LBIN();
                if (e_dec(&e, lctx, inc)) goto levels;
            }
        }
    }
    sig[n++] = (uint8_t)(maxn - 1);
levels:;
    int gt1 = 0, eq1 = 0;
    uint8_t *actx = ctx + 227 + abs_off[cat];
    const int lim = 4 - (cat == 3);
    for (int j = 0; j < n; j++) {
        int a;
        LBIN();
        if (!e_dec(&e, actx, gt1 ? 0 : (eq1 < 3 ? 1 + eq1 : 4))) { a = 1; eq1++; }
        else {
            int inc2 = 5 + (gt1 < lim ? gt1 : lim);
            int v = 1;
            while (v < 14) { LBIN(); if (!e_dec(&e, actx, inc2)) break; v++; }
            if (v == 14) {                                /* UEG0 suffix */
                int kk = 0; uint32_t sfx = 0;
                while (e_bypass(&e)) { LBIN(); sfx += 1u << kk; if (++kk > 24) return -1; }
                LBIN();
                while (kk--) { LBIN(); sfx += (uint32_t)e_bypass(&e) << kk; }
                v += (int)sfx;
            }
            a = v + 1;
            gt1++;
        }
        k->pos[j] = sig[n - 1 - j];
        LBIN();
        k->lev[j] = (int16_t)(e_bypass(&e) ? -a : a);
    }
    k->n = (uint8_t)n;
    *E = e;
#if C2C_STATS
    c->st.bins += bins;
#endif
#undef LBIN
    return n;
}

/* ------------------------------------------------------------------ CAVLC residual writer (9.2) */
static int cav_block(bw_t *w, const blk_t *k, int maxn, int nC)
{
    int tc = k->n, t1 = 0;
    const int16_t *lev = k->lev;
    const uint8_t *pos = k->pos;
    int tab = nC < 0 ? 4 : nC < 2 ? 0 : nC < 4 ? 1 : nC < 8 ? 2 : 3;
    if (!tc) { bw_put(w, coeff0_token[tab].code, coeff0_token[tab].len); return 0; }
    while (t1 < tc && t1 < 3 && (lev[t1] == 1 || lev[t1] == -1)) t1++;
    const vlc_t *ct = &coeff_token[tab][tc - 1][t1];
    if (t1) {                                            /* coeff_token + trailing_ones_sign_flags */
        uint32_t sg = 0;
        for (int i = 0; i < t1; i++) sg = (sg << 1) | (lev[i] < 0);
        bw_put(w, ((uint32_t)ct->code << t1) | sg, ct->len + t1);
    } else bw_put(w, ct->code, ct->len);
    int sl = (tc > 10 && t1 < 3) ? 1 : 0;
    for (int i = t1; i < tc; i++) {
        int l = lev[i];
        int adj = (i == t1 && t1 < 3) ? 2 : 0;
        int code = (l > 0 ? 2 * l - 2 : -2 * l - 1) - adj;
        if (sl == 0 && code < 14) bw_put(w, 1, code + 1);
        else if (sl == 0 && code < 30) bw_put(w, 16 | (code - 14), 19);
        else if (sl && code < (15 << sl)) {
            int pre = code >> sl;
            if (pre + 1 + sl <= 24) bw_put(w, (1u << sl) | (code & ((1 << sl) - 1)), pre + 1 + sl);
            else { bw_put(w, 1, pre + 1); bw_put(w, code & ((1 << sl) - 1), sl); }
        } else {
            /* level_prefix 15: 12-bit suffix (all a Baseline / Main decoder reads) */
            int base = (15 << sl) + (sl ? 0 : 15), rem = code - base;
            if (rem < 4096) { bw_put(w, 1, 16); bw_put(w, rem, 12); }
            else if (w->big && rem < 12288) {
                /* level_prefix 16 (a High-profile escape; only if the decoder reads it) */
                bw_put(w, 1, 17); bw_put(w, rem - 4096, 13);
                w->escaped++;
            } else {
                /* too large for CAVLC Main: the largest level of the same sign (tiny local error) */
                int c2 = base + 4095;
                if ((c2 & 1) != (code & 1)) c2--;
                code = c2;
                bw_put(w, 1, 16); bw_put(w, code - base, 12);
                int lc = code + adj;
                l = (lc & 1) ? -((lc + 1) >> 1) : (lc + 2) >> 1;
                w->clamped++;
            }
        }
        if (sl == 0) sl = 1;
        int al = l < 0 ? -l : l;
        if (al > (3 << (sl - 1)) && sl < 6) sl++;
    }
    if (tc < maxn) {
        int tz = pos[0] + 1 - tc;
        const vlc_t *z = maxn == 4 ? &total_zeros_dc[tc - 1][tz] : &total_zeros[tc - 1][tz];
        bw_put(w, z->code, z->len);
        for (int i = 0; i < tc - 1 && tz > 0; i++) {
            int run = pos[i] - pos[i + 1] - 1;
            const vlc_t *r = &run_before[(tz < 7 ? tz : 7) - 1][run];
            bw_put(w, r->code, r->len);
            tz -= run;
        }
    }
    return tc;
}

/* nC (9.2.1) from the TotalCoeff of the left and upper blocks, -1 = not available */
static inline int nc_of(int a, int b)
{
    if (a >= 0 && b >= 0) return (a + b + 1) >> 1;
    return a >= 0 ? a : b >= 0 ? b : 0;
}

/* ------------------------------------------------------------------ one macroblock */
typedef struct {
    int mbtype;                /* slice numbering: P 0..3 inter, 5.. intra; I 0..25 */
    int itype;                 /* 0 I_NxN, 1..24 I_16x16, 25 I_PCM; -1 inter */
    int sub[4], ref[4], np;
    int nmvd;
    int16_t mvd[16][2];
    uint8_t pflag[16], rem[16];
    int cmode, cbp, qpd;
    blk_t ldc, l[16], cdc[2], cac[2][4];
    uint8_t pcm[384];
} mbd_t;

typedef struct {
    int slice_type;            /* 0 P, 2 I */
    int num_ref;               /* num_ref_idx_l0_active */
    int last_qpd_nz;
} sctx_t;

static const uint8_t part_num[4] = { 1, 2, 2, 4 };
static const uint8_t sub_num[4] = { 1, 2, 2, 4 };

/* neighbour columns / rows into the caches */
static inline void col_in(uint32_t *w, uint32_t v)        /* column -1 of rows 0..3 */
{
    uint8_t *b = B8(w);
    b[I8(-1, 0)] = (uint8_t)v; b[I8(-1, 1)] = (uint8_t)(v >> 8); b[I8(-1, 2)] = (uint8_t)(v >> 16); b[I8(-1, 3)] = (uint8_t)(v >> 24);
}
static inline uint32_t col_out(const uint32_t *w)         /* column 3 of rows 0..3 */
{
    const uint8_t *b = (const uint8_t *)w;
    return b[I8(3, 0)] | (b[I8(3, 1)] << 8) | (b[I8(3, 2)] << 16) | ((uint32_t)b[I8(3, 3)] << 24);
}
static void mc_fill_inter(mc_t *m)
{
    const mbi_t *A = m->A, *B = m->B;
    if (m->avA) { col_in(m->mvx_w, A->mvx_r); col_in(m->mvy_w, A->mvy_r); col_in(m->refp_w, A->refp_r); }
    else { col_in(m->mvx_w, 0); col_in(m->mvy_w, 0); col_in(m->refp_w, 0); }
    if (m->avB) { m->mvx_w[1] = B->mvx_b; m->mvy_w[1] = B->mvy_b; m->refp_w[1] = B->refp_b; }
    else m->mvx_w[1] = m->mvy_w[1] = m->refp_w[1] = 0;
}
static void mc_fill_resid(mc_t *m)
{
    const mbi_t *A = m->A, *B = m->B;
    uint32_t una = m->intra ? 0x01010101u : 0;            /* coded_block_flag of a missing neighbour */
    col_in(m->cbf_w, m->avA ? A->cbf_r : una);
    col_in(m->nz_w, m->avA ? A->nz_r : 0xffffffffu);
    m->cbf_w[1] = m->avB ? B->cbf_b : una;
    m->nz_w[1] = m->avB ? B->nz_b : 0xffffffffu;
    m->cbf_w[3] = m->cbf_w[5] = m->cbf_w[7] = m->cbf_w[9] = 0;
    m->nz_w[3] = m->nz_w[5] = m->nz_w[7] = m->nz_w[9] = 0;
    uint32_t ca = m->avA ? A->ccbf_r : una, cb = m->avB ? B->ccbf_b : una;
    uint32_t na = m->avA ? A->cnz_r : 0xffffffffu, nb = m->avB ? B->cnz_b : 0xffffffffu;
    for (int c = 0; c < 2; c++) {
        uint8_t *f = m->ccbf[c]; int8_t *z = m->cnz[c];
        f[I4(-1, 0)] = (uint8_t)(ca >> (16 * c)); f[I4(-1, 1)] = (uint8_t)(ca >> (16 * c + 8));
        f[I4(0, -1)] = (uint8_t)(cb >> (16 * c)); f[I4(1, -1)] = (uint8_t)(cb >> (16 * c + 8));
        z[I4(-1, 0)] = (int8_t)(na >> (16 * c)); z[I4(-1, 1)] = (int8_t)(na >> (16 * c + 8));
        z[I4(0, -1)] = (int8_t)(nb >> (16 * c)); z[I4(1, -1)] = (int8_t)(nb >> (16 * c + 8));
        f[I4(0, 0)] = f[I4(1, 0)] = f[I4(0, 1)] = f[I4(1, 1)] = 0;
        z[I4(0, 0)] = z[I4(1, 0)] = z[I4(0, 1)] = z[I4(1, 1)] = 0;
    }
}
/* store the right column / bottom row of the current MB for later neighbours */
static void mc_store(mc_t *m, int inter, int resid)
{
    mbi_t *c = m->cur;
    if (inter) {
        c->mvx_r = col_out(m->mvx_w); c->mvx_b = m->mvx_w[9];
        c->mvy_r = col_out(m->mvy_w); c->mvy_b = m->mvy_w[9];
        c->refp_r = col_out(m->refp_w); c->refp_b = m->refp_w[9];
    } else c->mvx_r = c->mvx_b = c->mvy_r = c->mvy_b = c->refp_r = c->refp_b = 0;
    if (resid) {
        c->cbf_r = col_out(m->cbf_w); c->cbf_b = m->cbf_w[9];
        c->nz_r = col_out(m->nz_w); c->nz_b = m->nz_w[9];
        const uint8_t *f0 = m->ccbf[0], *f1 = m->ccbf[1];
        const uint8_t *z0 = (const uint8_t *)m->cnz[0], *z1 = (const uint8_t *)m->cnz[1];
        c->ccbf_r = f0[I4(1, 0)] | (f0[I4(1, 1)] << 8) | (f1[I4(1, 0)] << 16) | ((uint32_t)f1[I4(1, 1)] << 24);
        c->ccbf_b = f0[I4(0, 1)] | (f0[I4(1, 1)] << 8) | (f1[I4(0, 1)] << 16) | ((uint32_t)f1[I4(1, 1)] << 24);
        c->cnz_r = z0[I4(1, 0)] | (z0[I4(1, 1)] << 8) | (z1[I4(1, 0)] << 16) | ((uint32_t)z1[I4(1, 1)] << 24);
        c->cnz_b = z0[I4(0, 1)] | (z0[I4(1, 1)] << 8) | (z1[I4(0, 1)] << 16) | ((uint32_t)z1[I4(1, 1)] << 24);
    } else c->cbf_r = c->cbf_b = c->nz_r = c->nz_b = c->ccbf_r = c->ccbf_b = c->cnz_r = c->cnz_b = 0;
}
static inline void mbi_set_const(mbi_t *c, uint32_t cbf, uint32_t nz)
{
    c->cbf_r = c->cbf_b = c->ccbf_r = c->ccbf_b = cbf;
    c->nz_r = c->nz_b = c->cnz_r = c->cnz_b = nz;
    c->mvx_r = c->mvx_b = c->mvy_r = c->mvy_b = c->refp_r = c->refp_b = 0;
}

static inline void fill_mvd(mc_t *m, int x4, int y4, int w4, int h4, int mx, int my)
{
    uint32_t ax = mx < 0 ? -mx : mx, ay = my < 0 ? -my : my;
    if (ax > 64) ax = 64;
    if (ay > 64) ay = 64;
    if (w4 == 4) {
        uint32_t wx = ax * 0x01010101u, wy = ay * 0x01010101u;
        for (int y = y4; y < y4 + h4; y++) { m->mvx_w[(y + 1) * 2 + 1] = wx; m->mvy_w[(y + 1) * 2 + 1] = wy; }
        return;
    }
    uint8_t *bx = B8(m->mvx_w), *by = B8(m->mvy_w);
    for (int y = y4; y < y4 + h4; y++)
        for (int x = x4; x < x4 + w4; x++) { bx[I8(x, y)] = (uint8_t)ax; by[I8(x, y)] = (uint8_t)ay; }
}
static inline void fill_ref(mc_t *m, int x4, int y4, int w4, int h4, int refp)
{
    if (w4 == 4) { for (int y = y4; y < y4 + h4; y++) m->refp_w[(y + 1) * 2 + 1] = refp ? 0x01010101u : 0; return; }
    uint8_t *b = B8(m->refp_w);
    for (int y = y4; y < y4 + h4; y++)
        for (int x = x4; x < x4 + w4; x++) b[I8(x, y)] = (uint8_t)refp;
}
static AI int cab_mvd(c2c_t *c, eng_t *E, const mc_t *m, int x, int y, int comp)
{
    int base = comp ? 47 : 40;
    const uint8_t *mv = comp ? (const uint8_t *)m->mvy_w : (const uint8_t *)m->mvx_w;
    int s = mv[I8(x - 1, y)] + mv[I8(x, y - 1)];
    int inc = s < 3 ? 0 : s > 32 ? 2 : 1;
    if (!cab_dec(c, base + inc)) return 0;
    int v = 1;
    while (v < 9 && cab_dec(c, base + (v < 4 ? v + 2 : 6))) v++;
    if (v == 9) { int s2 = cab_ue_k(c, E, 3); if (s2 < 0 || s2 > 8192) return 0x7fff; v += s2; }
    return cab_bypass(c) ? -v : v;
}
static AI int cab_ref(c2c_t *c, eng_t *E, const mc_t *m, int x, int y)
{
    const uint8_t *rp = (const uint8_t *)m->refp_w;
    int inc = rp[I8(x - 1, y)] + 2 * rp[I8(x, y - 1)];
    if (!cab_dec(c, 54 + inc)) return 0;
    int v = 1;
    while (cab_dec(c, 54 + (v == 1 ? 4 : 5))) { if (++v > 32) return -1; }
    return v;
}
static void part_geom(int mbtype, int p, int *x, int *y, int *w, int *h)
{
    if (mbtype == 0) { *x = 0; *y = 0; *w = 4; *h = 4; }
    else if (mbtype == 1) { *x = 0; *y = p * 2; *w = 4; *h = 2; }     /* 16x8 */
    else { *x = p * 2; *y = 0; *w = 2; *h = 4; }                      /* 8x16 */
}
static void sub_geom(int sub, int s, int *x, int *y, int *w, int *h)
{
    if (sub == 0) { *x = 0; *y = 0; *w = 2; *h = 2; }
    else if (sub == 1) { *x = 0; *y = s; *w = 2; *h = 1; }            /* 8x4 */
    else if (sub == 2) { *x = s; *y = 0; *w = 1; *h = 2; }            /* 4x8 */
    else { *x = s & 1; *y = s >> 1; *w = 1; *h = 1; }
}

/* intra mb_type bins after the first one (9.3.2.5, ctxIdx per table 9-39) */
static AI int cab_itype(c2c_t *c, eng_t *E, int c0, int cl, int cc, int cc2, int cp1, int cp0)
{
    if (!cab_dec(c, c0)) return 0;                       /* I_NxN */
    if (cab_term(c, E)) return 25;                          /* I_PCM */
    int luma = cab_dec(c, cl), chroma = 0;
    if (cab_dec(c, cc)) chroma = 1 + cab_dec(c, cc2);
    int pm = cab_dec(c, cp1) << 1;
    pm |= cab_dec(c, cp0);
    return 1 + pm + 4 * chroma + 12 * luma;
}

static int cbp_luma_cond(const mbi_t *n, int avail, int b8)       /* 1 when that 8x8 had no residual */
{
    if (!avail || n->type == T_PCM) return 0;
    return !((n->cbp >> b8) & 1);
}
static int cbp_chroma_cond(const mbi_t *n, int avail, int bin)
{
    if (!avail) return 0;
    if (n->type == T_PCM) return 1;
    if (n->type == T_SKIP) return 0;
    return bin ? (n->cbp >> 4) == 2 : (n->cbp >> 4) != 0;
}
static inline int dc_cond(const mbi_t *n, int avail, int intra, int bit)
{
    return avail ? (n->dc >> bit) & 1 : intra;
}

/* macroblock_layer after mb_skip_flag == 0, CABAC -> d and the caches */
static int cab_mb_(c2c_t *c, eng_t *E, sctx_t *s, mc_t *m, mbd_t *d);
static int cab_mb(c2c_t *c, eng_t *Eo, sctx_t *s, mc_t *m, mbd_t *d)
{
    eng_t e = *Eo;
    int r = cab_mb_(c, &e, s, m, d);
    *Eo = e;
    return r;
}
static AI int cab_mb_(c2c_t *c, eng_t *E, sctx_t *s, mc_t *m, mbd_t *d)
{
    mbi_t *cur = m->cur;
    int itype = -1;
    if (s->slice_type == 0) {
        if (!cab_dec(c, 14)) {
            if (!cab_dec(c, 15)) d->mbtype = cab_dec(c, 16) ? 3 : 0;
            else d->mbtype = cab_dec(c, 17) ? 1 : 2;
        } else {
            itype = cab_itype(c, E, 17, 18, 19, 19, 20, 20);
            d->mbtype = 5 + itype;
        }
    } else {
        int inc = (m->avA && m->A->type != T_I4) + (m->avB && m->B->type != T_I4);
        itype = cab_itype(c, E, 3 + inc, 6, 7, 8, 9, 10);
        d->mbtype = itype;
    }
    d->itype = itype;
    m->intra = itype >= 0;

    if (itype == 25) {                                    /* I_PCM (7.3.5, 9.3.1.2) */
        uint32_t pos = (cab_pos(c, E) + 7) & ~7u;          /* pcm_alignment_zero_bit */
        if (pos + 384 * 8 > c->b->end) return C2C_ERR_SYNTAX;
        memcpy(d->pcm, c->b->buf + (pos >> 3), 384);
        cab_init_engine(c, E, pos + 384 * 8);
        cur->type = T_PCM; cur->cbp = 0x2f; cur->dc = 7; cur->cmode = 0;
        mbi_set_const(cur, 0x01010101u, 0x10101010u);
        s->last_qpd_nz = 0;
        return 0;
    }

    if (itype < 0) {                                      /* inter prediction */
        cur->type = T_P;
        mc_fill_inter(m);
        if (d->mbtype == 3) {
            for (int i = 0; i < 4; i++) {
                if (cab_dec(c, 21)) d->sub[i] = 0;
                else if (!cab_dec(c, 22)) d->sub[i] = 1;
                else d->sub[i] = cab_dec(c, 23) ? 2 : 3;
            }
            for (int i = 0; i < 4; i++) {
                int x = (i & 1) * 2, y = (i >> 1) * 2;
                d->ref[i] = s->num_ref > 1 ? cab_ref(c, E, m, x, y) : 0;
                if (d->ref[i] < 0 || d->ref[i] >= s->num_ref) return C2C_ERR_SYNTAX;
                fill_ref(m, x, y, 2, 2, d->ref[i] > 0);
            }
            d->nmvd = 0;
            for (int i = 0; i < 4; i++)
                for (int k = 0; k < sub_num[d->sub[i]]; k++) {
                    int x, y, w, h;
                    sub_geom(d->sub[i], k, &x, &y, &w, &h);
                    x += (i & 1) * 2; y += (i >> 1) * 2;
                    int mx = cab_mvd(c, E, m, x, y, 0), my = cab_mvd(c, E, m, x, y, 1);
                    if (mx == 0x7fff || my == 0x7fff) return C2C_ERR_SYNTAX;
                    d->mvd[d->nmvd][0] = (int16_t)mx; d->mvd[d->nmvd][1] = (int16_t)my; d->nmvd++;
                    fill_mvd(m, x, y, w, h, mx, my);
                }
        } else {
            d->np = part_num[d->mbtype];
            for (int p = 0; p < d->np; p++) {
                int x, y, w, h;
                part_geom(d->mbtype, p, &x, &y, &w, &h);
                d->ref[p] = s->num_ref > 1 ? cab_ref(c, E, m, x, y) : 0;
                if (d->ref[p] < 0 || d->ref[p] >= s->num_ref) return C2C_ERR_SYNTAX;
                fill_ref(m, x, y, w, h, d->ref[p] > 0);
            }
            d->nmvd = 0;
            for (int p = 0; p < d->np; p++) {
                int x, y, w, h;
                part_geom(d->mbtype, p, &x, &y, &w, &h);
                int mx = cab_mvd(c, E, m, x, y, 0), my = cab_mvd(c, E, m, x, y, 1);
                if (mx == 0x7fff || my == 0x7fff) return C2C_ERR_SYNTAX;
                d->mvd[d->nmvd][0] = (int16_t)mx; d->mvd[d->nmvd][1] = (int16_t)my; d->nmvd++;
                fill_mvd(m, x, y, w, h, mx, my);
            }
        }
        cur->cmode = 0;
    } else {                                              /* intra prediction */
        cur->type = itype == 0 ? T_I4 : T_I16;
        if (itype == 0)
            for (int i = 0; i < 16; i++) {
                d->pflag[i] = (uint8_t)cab_dec(c, 68);
                if (!d->pflag[i]) {
                    int v = cab_dec(c, 69);
                    v |= cab_dec(c, 69) << 1;
                    v |= cab_dec(c, 69) << 2;
                    d->rem[i] = (uint8_t)v;
                }
            }
        int ca = m->avA && (m->A->type == T_I4 || m->A->type == T_I16) && m->A->cmode;
        int cb = m->avB && (m->B->type == T_I4 || m->B->type == T_I16) && m->B->cmode;
        int v = 0;
        if (cab_dec(c, 64 + ca + cb)) { v = 1; if (cab_dec(c, 67)) { v = 2; if (cab_dec(c, 67)) v = 3; } }
        d->cmode = v;
        cur->cmode = (uint8_t)v;
    }

    /* coded_block_pattern */
    if (itype >= 1) {
        int t = itype - 1;
        d->cbp = ((t / 12) ? 15 : 0) | (((t / 4) % 3) << 4);
    } else {
        int cbp = 0;
        for (int b8 = 0; b8 < 4; b8++) {
            int a = (b8 & 1) ? !((cbp >> (b8 - 1)) & 1) : cbp_luma_cond(m->A, m->avA, b8 + 1);
            int b = (b8 & 2) ? !((cbp >> (b8 - 2)) & 1) : cbp_luma_cond(m->B, m->avB, b8 + 2);
            cbp |= cab_dec(c, 73 + a + 2 * b) << b8;
        }
        int inc = cbp_chroma_cond(m->A, m->avA, 0) + 2 * cbp_chroma_cond(m->B, m->avB, 0);
        if (cab_dec(c, 77 + inc)) {
            inc = cbp_chroma_cond(m->A, m->avA, 1) + 2 * cbp_chroma_cond(m->B, m->avB, 1);
            cbp |= (1 + cab_dec(c, 81 + inc)) << 4;
        }
        d->cbp = cbp;
    }
    cur->cbp = (uint8_t)d->cbp;
    cur->dc = 0;

    d->qpd = 0;
    if (!d->cbp && itype < 1) {
        s->last_qpd_nz = 0;
        mc_store(m, itype < 0, 0);
        return 0;
    }
    if (cab_dec(c, 60 + s->last_qpd_nz)) {
        int k = 1;
        while (cab_dec(c, 60 + (k == 1 ? 2 : 3))) { if (++k > 104) return C2C_ERR_SYNTAX; }
        d->qpd = (k & 1) ? (k + 1) >> 1 : -(k >> 1);
    }
    s->last_qpd_nz = d->qpd != 0;

    /* residual (7.3.5.3) */
    mc_fill_resid(m);
    if (itype >= 1) {
        int inc = dc_cond(m->A, m->avA, 1, 0) + 2 * dc_cond(m->B, m->avB, 1, 0);
        if (cab_dec(c, 85 + 0 + inc)) {
            if (cab_block(c, E, 0, 16, &d->ldc) < 0) return C2C_ERR_SYNTAX;
            cur->dc |= 1;
        } else d->ldc.n = 0;
        for (int blk = 0; blk < 16; blk++) {
            d->l[blk].n = 0;
            if (!(d->cbp & 15)) continue;
            int x = blkx[blk], y = blky[blk];
            if (cab_dec(c, 85 + 4 + B8(m->cbf_w)[I8(x - 1, y)] + 2 * B8(m->cbf_w)[I8(x, y - 1)])) {
                int n = cab_block(c, E, 1, 15, &d->l[blk]);
                if (n < 0) return C2C_ERR_SYNTAX;
                B8(m->cbf_w)[I8(x, y)] = 1; B8(m->nz_w)[I8(x, y)] = (uint8_t)n;
            }
        }
    } else {
        for (int blk = 0; blk < 16; blk++) {
            d->l[blk].n = 0;
            if (!((d->cbp >> (blk >> 2)) & 1)) continue;
            int x = blkx[blk], y = blky[blk];
            if (cab_dec(c, 85 + 8 + B8(m->cbf_w)[I8(x - 1, y)] + 2 * B8(m->cbf_w)[I8(x, y - 1)])) {
                int n = cab_block(c, E, 2, 16, &d->l[blk]);
                if (n < 0) return C2C_ERR_SYNTAX;
                B8(m->cbf_w)[I8(x, y)] = 1; B8(m->nz_w)[I8(x, y)] = (uint8_t)n;
            }
        }
    }
    d->cdc[0].n = d->cdc[1].n = 0;
    for (int i = 0; i < 4; i++) d->cac[0][i].n = d->cac[1][i].n = 0;
    if (d->cbp >> 4) {
        for (int comp = 0; comp < 2; comp++) {
            int inc = dc_cond(m->A, m->avA, m->intra, 1 + comp) + 2 * dc_cond(m->B, m->avB, m->intra, 1 + comp);
            if (cab_dec(c, 85 + 12 + inc)) {
                if (cab_block(c, E, 3, 4, &d->cdc[comp]) < 0) return C2C_ERR_SYNTAX;
                cur->dc |= 2 << comp;
            }
        }
        if ((d->cbp >> 4) == 2)
            for (int comp = 0; comp < 2; comp++)
                for (int b = 0; b < 4; b++) {
                    int x = b & 1, y = b >> 1;
                    uint8_t *cf = m->ccbf[comp];
                    if (cab_dec(c, 85 + 16 + cf[I4(x - 1, y)] + 2 * cf[I4(x, y - 1)])) {
                        int n = cab_block(c, E, 4, 15, &d->cac[comp][b]);
                        if (n < 0) return C2C_ERR_SYNTAX;
                        cf[I4(x, y)] = 1; m->cnz[comp][I4(x, y)] = (int8_t)n;
                    }
                }
    }
    mc_store(m, itype < 0, 1);
    return 0;
}

static void bw_te(bw_t *w, int v, int range) { if (range == 1) bw_put(w, !v, 1); else bw_ue(w, v); }

/* macroblock_layer in CAVLC */
static int cav_mb(bw_t *w, sctx_t *s, mc_t *m, mbd_t *d)
{
    bw_ue(w, d->mbtype);
    if (d->itype == 25) {
        { int used = (32 - w->left) & 7; if (used) bw_put(w, 0, 8 - used); }
        for (int i = 0; i < 384; i++) bw_put(w, d->pcm[i], 8);
        return 0;
    }
    if (d->itype < 0) {
        if (d->mbtype == 3) {
            for (int i = 0; i < 4; i++) bw_ue(w, d->sub[i]);
            if (s->num_ref > 1) for (int i = 0; i < 4; i++) bw_te(w, d->ref[i], s->num_ref - 1);
        } else if (s->num_ref > 1)
            for (int p = 0; p < d->np; p++) bw_te(w, d->ref[p], s->num_ref - 1);
        for (int i = 0; i < d->nmvd; i++) { bw_se(w, d->mvd[i][0]); bw_se(w, d->mvd[i][1]); }
    } else {
        if (d->itype == 0)
            for (int i = 0; i < 16; i++) {
                if (d->pflag[i]) bw_put(w, 1, 1); else bw_put(w, d->rem[i], 4);
            }
        bw_ue(w, d->cmode);
    }
    if (d->itype < 1)
        bw_ue(w, d->itype == 0 ? cbp_to_golomb_intra[d->cbp] : cbp_to_golomb_inter[d->cbp]);
    if (!d->cbp && d->itype < 1) return 0;
    bw_se(w, d->qpd);
    if (d->itype >= 1) {
        if (cav_block(w, &d->ldc, 16, nc_of(((int8_t *)m->nz_w)[I8(-1, 0)], ((int8_t *)m->nz_w)[I8(0, -1)])) < 0) return C2C_ERR_LEVEL;
        if (d->cbp & 15)
            for (int blk = 0; blk < 16; blk++) {
                int x = blkx[blk], y = blky[blk];
                if (cav_block(w, &d->l[blk], 15, nc_of(((int8_t *)m->nz_w)[I8(x - 1, y)], ((int8_t *)m->nz_w)[I8(x, y - 1)])) < 0) return C2C_ERR_LEVEL;
            }
    } else {
        for (int blk = 0; blk < 16; blk++)
            if ((d->cbp >> (blk >> 2)) & 1) {
                int x = blkx[blk], y = blky[blk];
                if (cav_block(w, &d->l[blk], 16, nc_of(((int8_t *)m->nz_w)[I8(x - 1, y)], ((int8_t *)m->nz_w)[I8(x, y - 1)])) < 0) return C2C_ERR_LEVEL;
            }
    }
    if (d->cbp >> 4) {
        for (int comp = 0; comp < 2; comp++)
            if (cav_block(w, &d->cdc[comp], 4, -1) < 0) return C2C_ERR_LEVEL;
        if ((d->cbp >> 4) == 2)
            for (int comp = 0; comp < 2; comp++)
                for (int b = 0; b < 4; b++) {
                    int x = b & 1, y = b >> 1;
                    const int8_t *nz = m->cnz[comp];
                    if (cav_block(w, &d->cac[comp][b], 15, nc_of(nz[I4(x - 1, y)], nz[I4(x, y - 1)])) < 0) return C2C_ERR_LEVEL;
                }
    }
    return 0;
}

/* ------------------------------------------------------------------ parameter set parsing */
static int rbsp_more(const br_t *b)                       /* more_rbsp_data() */
{
    uint32_t last = b->end;                                /* position after the last 1 bit */
    while (last > b->pos) {
        uint32_t p = last - 1;
        if ((b->buf[p >> 3] >> (7 - (p & 7))) & 1) break;
        last--;
    }
    return last > b->pos + 1;
}

static int parse_sps(c2c_t *c, br_t *b)
{
    sps_t t; memset(&t, 0, sizeof t);
    t.profile = (uint8_t)br_u(b, 8); br_u(b, 16);
    uint32_t id = br_ue(b);
    if (id > 31) return C2C_ERR_SYNTAX;
    int p = t.profile;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128
        || p == 138 || p == 139 || p == 134 || p == 135) { c->sps[id].valid = 2; return C2C_OK; }   /* High: refused at slices */
    t.log2_max_frame_num = (uint8_t)(br_ue(b) + 4);
    t.poc_type = (uint8_t)br_ue(b);
    if (t.poc_type == 0) t.log2_max_poc_lsb = (uint8_t)(br_ue(b) + 4);
    else if (t.poc_type == 1) {
        t.delta_pic_order_always_zero = (uint8_t)br_u1(b);
        br_se(b); br_se(b);
        uint32_t n = br_ue(b);
        if (n > 255) return C2C_ERR_SYNTAX;
        while (n--) br_se(b);
    }
    br_ue(b); br_u1(b);                                    /* max_num_ref_frames, gaps */
    t.mb_w = (uint16_t)(br_ue(b) + 1);
    uint32_t hmu = br_ue(b) + 1;
    t.frame_mbs_only = (uint8_t)br_u1(b);
    t.mb_h = (uint16_t)(hmu * (2 - t.frame_mbs_only));
    t.valid = (t.frame_mbs_only && (uint32_t)t.mb_w * t.mb_h <= C2C_MAX_MBS) ? 1 : 2;
    if (b->pos > b->end) return C2C_ERR_SYNTAX;
    c->sps[id] = t;
    return C2C_OK;
}

static int parse_pps(c2c_t *c, br_t *b, pps_t *t, uint32_t *id, int32_t *qs, int32_t *cqp, uint8_t *wbi, uint8_t *cip)
{
    (void)c;
    memset(t, 0, sizeof *t);
    *id = br_ue(b);
    uint32_t sid = br_ue(b);
    if (*id > 255 || sid > 31) return C2C_ERR_SYNTAX;
    t->sps_id = (uint8_t)sid;
    t->cabac = (uint8_t)br_u1(b);
    t->bottom_field_pic_order = (uint8_t)br_u1(b);
    int groups = (int)br_ue(b);
    t->valid = 1;
    if (groups) { t->valid = 2; return C2C_OK; }           /* slice groups (FMO): not Main */
    t->num_ref_l0 = (uint8_t)(br_ue(b) + 1);
    t->num_ref_l1 = (uint8_t)(br_ue(b) + 1);
    t->weighted_pred = (uint8_t)br_u1(b);
    *wbi = (uint8_t)br_u(b, 2);
    t->init_qp = (int8_t)br_se(b);
    *qs = br_se(b);
    *cqp = br_se(b);
    t->deblock_ctrl = (uint8_t)br_u1(b);
    *cip = (uint8_t)br_u1(b);
    t->redundant_cnt = (uint8_t)br_u1(b);
    if (b->pos > b->end) return C2C_ERR_SYNTAX;
    if (rbsp_more(b)) {                                    /* High-profile extension */
        int t8 = br_u1(b), sm = br_u1(b);
        int32_t second = 0;
        if (!sm) second = br_se(b);
        if (t8 || sm || second != *cqp) t->valid = 2;
    }
    return C2C_OK;
}

/* ------------------------------------------------------------------ slices */
static int do_slice(c2c_t *c, int nal_ref_idc, int nal_type, br_t *b, bw_t *w)
{
    uint32_t first_mb = br_ue(b);
    uint32_t st = br_ue(b) % 5;
    uint32_t pid = br_ue(b);
    if (pid > 255 || !c->pps[pid].valid) return C2C_ERR_NOPARAM;
    pps_t *pp = &c->pps[pid];
    sps_t *sp = &c->sps[pp->sps_id];
    if (!sp->valid) return C2C_ERR_NOPARAM;
    if (pp->valid != 1 || sp->valid != 1 || (st != 0 && st != 2)) return C2C_ERR_UNSUPPORTED;
    br_u(b, sp->log2_max_frame_num);
    if (nal_type == 5) br_ue(b);
    if (sp->poc_type == 0) {
        br_u(b, sp->log2_max_poc_lsb);
        if (pp->bottom_field_pic_order) br_se(b);
    } else if (sp->poc_type == 1 && !sp->delta_pic_order_always_zero) {
        br_se(b);
        if (pp->bottom_field_pic_order) br_se(b);
    }
    if (pp->redundant_cnt) br_ue(b);
    sctx_t s; s.slice_type = (int)st; s.num_ref = pp->num_ref_l0; s.last_qpd_nz = 0;
    if (st == 0) {
        if (br_u1(b)) s.num_ref = (int)br_ue(b) + 1;
        if (s.num_ref > 32) return C2C_ERR_SYNTAX;
        if (br_u1(b))
            for (int n = 0;; n++) {
                uint32_t idc = br_ue(b);
                if (idc == 3) break;
                if (idc > 3 || n > 64) return C2C_ERR_SYNTAX;
                br_ue(b);
            }
        if (pp->weighted_pred) return C2C_ERR_UNSUPPORTED;
    }
    if (nal_ref_idc) {
        if (nal_type == 5) { br_u1(b); br_u1(b); }
        else if (br_u1(b))
            for (int n = 0;; n++) {
                uint32_t op = br_ue(b);
                if (op == 0) break;
                if (op > 6 || n > 64) return C2C_ERR_SYNTAX;
                if (op == 1 || op == 3) br_ue(b);
                if (op == 2) br_ue(b);
                if (op == 3 || op == 6) br_ue(b);
                if (op == 4) br_ue(b);
            }
    }
    uint32_t pos_ci = b->pos, idc = 0;
    if (st != 2) { idc = br_ue(b); if (idc > 2) return C2C_ERR_SYNTAX; }
    uint32_t pos_after = b->pos;
    int qp = 26 + pp->init_qp + br_se(b);
    if (pp->deblock_ctrl) { if (br_ue(b) != 1) { br_se(b); br_se(b); } }
    uint32_t hdr_end = b->pos;
    if (hdr_end > b->end) return C2C_ERR_SYNTAX;

    /* header without cabac_init_idc */
    bw_copy(w, b, 0, pos_ci);
    bw_copy(w, b, pos_after, hdr_end);

    /* slice_data */
    b->pos = (hdr_end + 7) & ~7u;                          /* cabac_alignment_one_bit */
    c->b = b;
    cab_init_ctx(c, qp, st == 2 ? 0 : 1 + idc);
    eng_t eng, *E = &eng;
    cab_init_engine(c, E, b->pos);

    uint32_t W = sp->mb_w, N = (uint32_t)sp->mb_w * sp->mb_h, addr = first_mb, skip_run = 0;
    uint32_t serial = ++c->slice_serial;
    uint32_t mbx = first_mb;
    if (first_mb >= N) return C2C_ERR_SYNTAX;
    while (mbx >= W) mbx -= W;
    mbd_t *dp = (mbd_t *)c->mbd;
    mc_t m;
    const uint8_t *pend = b->buf + (b->end >> 3) + 8;      /* reading past this = broken slice */
    for (;;) {
        if (addr >= N || E->p > pend) return C2C_ERR_SYNTAX;
        mbi_t *cur = &c->mb[addr];
        cur->slice = serial;
        m.cur = cur;
        /* slices are in raster order (no ASO in Main): a neighbour is in this slice iff it is not before first_mb */
        m.avA = mbx && addr > first_mb;
        m.avB = addr >= first_mb + W;
        m.A = &c->mb[m.avA ? addr - 1 : addr];
        m.B = &c->mb[m.avB ? addr - W : addr];
        m.intra = 0;
        int skip = 0;
        if (st == 0) {
            int inc = (m.avA && m.A->type != T_SKIP) + (m.avB && m.B->type != T_SKIP);
            skip = cab_dec(c, 11 + inc);
        }
        c->st.mbs++;
        if (skip) {
            cur->type = T_SKIP; cur->cbp = 0; cur->cmode = 0; cur->dc = 0;
            mbi_set_const(cur, 0, 0);
            skip_run++;
            s.last_qpd_nz = 0;
            c->st.skipped++;
        } else {
            if (st == 0) { bw_ue(w, skip_run); skip_run = 0; }
            int e = cab_mb(c, E, &s, &m, dp);
            if (e) return e;
#ifndef C2C_NOWRITE
            e = cav_mb(w, &s, &m, dp);
#endif
            if (e) return e;
        }
        addr++;
        if (++mbx == W) mbx = 0;
        if (cab_term(c, E)) break;                            /* end_of_slice_flag */
    }
    if (skip_run) bw_ue(w, skip_run);
    bw_trailing(w);
    c->st.slices++;
    return C2C_OK;
}

/* ------------------------------------------------------------------ NAL units */
int c2c_nal(c2c_t *c, const uint8_t *in, int in_len, uint8_t *out, int out_cap, uint8_t *rbsp)
{
    if (in_len < 1) return C2C_ERR_SYNTAX;
    int type = in[0] & 0x1f, ref = (in[0] >> 5) & 3;
    c->st.nals++;
    c->st.in_bytes += in_len;
    if (type != 1 && type != 5 && type != 7 && type != 8) {
copy:
        if (in_len > out_cap) return C2C_ERR_SPACE;
        memcpy(out, in, in_len);
        c->st.out_bytes += in_len;
        return in_len;
    }
    /* RBSP: drop emulation_prevention_three_byte */
    int n = 0, z = 0;
    for (int i = 1; i < in_len; i++) {
        if (z >= 2 && in[i] == 3) { z = 0; continue; }
        rbsp[n++] = in[i];
        z = in[i] ? 0 : z + 1;
    }
    memset(rbsp + n, 0, 16);
    br_t b = { rbsp, 0, (uint32_t)n * 8 };
    bw_t w = { c->obuf, c->obuf + sizeof c->obuf, 0, 32, 0, c->big, 0, 0 };
    if (out_cap < 2) return C2C_ERR_SPACE;
    out[0] = in[0];

    if (type == 7) { int e = parse_sps(c, &b); if (e) return e; goto copy; }
    if (type == 8) {
        pps_t t; uint32_t id; int32_t qs, cqp; uint8_t wbi, cip;
        int e = parse_pps(c, &b, &t, &id, &qs, &cqp, &wbi, &cip);
        if (e) return e;
        c->pps[id] = t;
        if (!t.cabac || t.valid != 1) goto copy;
        bw_ue(&w, id); bw_ue(&w, t.sps_id);
        bw_put(&w, 0, 1);                                  /* entropy_coding_mode_flag = 0 (CAVLC) */
        bw_put(&w, t.bottom_field_pic_order, 1);
        bw_ue(&w, 0);
        bw_ue(&w, t.num_ref_l0 - 1); bw_ue(&w, t.num_ref_l1 - 1);
        bw_put(&w, t.weighted_pred, 1); bw_put(&w, wbi, 2);
        bw_se(&w, t.init_qp); bw_se(&w, qs); bw_se(&w, cqp);
        bw_put(&w, t.deblock_ctrl, 1); bw_put(&w, cip, 1); bw_put(&w, t.redundant_cnt, 1);
        bw_trailing(&w);
    } else {
        /* slice: transcode only when its PPS uses CABAC */
        br_t pk = b;
        br_ue(&pk); br_ue(&pk);
        uint32_t pid = br_ue(&pk);
        if (pid > 255 || !c->pps[pid].valid) return C2C_ERR_NOPARAM;
        if (!c->pps[pid].cabac) goto copy;
        int e = do_slice(c, ref, type, &b, &w);
        if (e) return e;
    }
    c->st.clamped += w.clamped;
    c->st.escaped += w.escaped;
    if (w.err) return C2C_ERR_SPACE;
    int len = escape(c->obuf, (int)(w.p - c->obuf), out + 1, out_cap - 1);
    if (len < 0) return C2C_ERR_SPACE;
    len++;
    c->st.out_bytes += len;
    return len;
}
/* the decoded-macroblock area must hold an mbd_t */
typedef char c2c_mbd_fits[(sizeof(mbd_t) <= sizeof(((struct c2c *)0)->mbd)) ? 1 : -1];
