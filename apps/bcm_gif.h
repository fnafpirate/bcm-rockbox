/* v142: a small GIF decoder for the TV-out animation on the iPod's screen (apps/debug_menu.c includes this file).
   GIF87a / GIF89a, global and local colour tables, transparency, interlacing, disposal 0-3.  The caller gives the
   byte reader, the work memory and a callback that gets every finished frame as a full ARGB canvas (transparent
   pixels = 0).  count_only = walk the file without decoding the pictures (frame count, size). */
#ifndef BCM_GIF_H
#define BCM_GIF_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef int  (*gif_read_fn)(void *ctx, void *buf, int n);                 /* bytes read, < n at the end */
typedef bool (*gif_frame_fn)(void *ctx, const uint32_t *canvas, int w, int h, int delay_cs, int index);

struct gif_work {                     /* ~16 KB, plus the two canvases the caller provides */
    uint16_t prefix[4096];
    uint8_t  suffix[4096];
    uint8_t  stack[4097];
    uint8_t  blk[256];
    uint32_t pal[256];
    uint32_t gpal[256];
};

struct gif_in { gif_read_fn rd; void *ctx; bool eof; };
static int gif_byte(struct gif_in *in)
{
    uint8_t b;
    if (in->eof || in->rd(in->ctx, &b, 1) != 1) { in->eof = true; return -1; }
    return b;
}
static int gif_u16(struct gif_in *in) { int a = gif_byte(in), b = gif_byte(in); return a < 0 || b < 0 ? -1 : a | b << 8; }
static bool gif_read_pal(struct gif_in *in, uint32_t *pal, int n)
{
    uint8_t rgb[3];
    for (int i = 0; i < n; i++)
    {
        if (in->rd(in->ctx, rgb, 3) != 3) { in->eof = true; return false; }
        pal[i] = 0xFF000000u | (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
    }
    return true;
}
static bool gif_skip_blocks(struct gif_in *in, uint8_t *blk)
{
    for (;;)
    {
        int n = gif_byte(in);
        if (n < 0) return false;
        if (n == 0) return true;
        if (in->rd(in->ctx, blk, n) != n) { in->eof = true; return false; }
    }
}

/* returns the number of frames (>= 0), or -1 not a GIF / too big, -2 broken */
static int gif_decode(gif_read_fn rd, void *rctx, struct gif_work *wk, uint32_t *canvas, uint32_t *prev, int max_px,
                      bool count_only, gif_frame_fn fr, void *fctx, int *pw, int *ph)
{
    struct gif_in in = { rd, rctx, false };
    uint8_t hdr[13];
    int W, H, flags, frames = 0, disposal = 0, delay = 0, transp = -1;
    if (rd(rctx, hdr, 13) != 13 || memcmp(hdr, "GIF8", 4) || (hdr[4] != '7' && hdr[4] != '9') || hdr[5] != 'a') return -1;
    W = hdr[6] | hdr[7] << 8; H = hdr[8] | hdr[9] << 8; flags = hdr[10];
    if (W <= 0 || H <= 0 || W * H > max_px) return -1;
    if (pw) *pw = W;
    if (ph) *ph = H;
    memset(wk->gpal, 0, sizeof wk->gpal);
    if ((flags & 0x80) && !gif_read_pal(&in, wk->gpal, 2 << (flags & 7))) return -2;
    if (!count_only) memset(canvas, 0, (size_t)W * H * 4);
    for (;;)
    {
        int c = gif_byte(&in);
        if (c < 0 || c == 0x3B) return frames;
        if (c == 0x21)
        {
            int label = gif_byte(&in);
            if (label == 0xF9)
            {
                int n = gif_byte(&in);
                if (n < 4 || rd(rctx, wk->blk, n) != n) return frames;
                disposal = (wk->blk[0] >> 2) & 7; delay = wk->blk[1] | wk->blk[2] << 8;
                transp = (wk->blk[0] & 1) ? wk->blk[3] : -1;
            }
            if (!gif_skip_blocks(&in, wk->blk)) return frames;
            continue;
        }
        if (c != 0x2C) return frames ? frames : -2;
        {
            int fx = gif_u16(&in), fy = gif_u16(&in), fw = gif_u16(&in), fh = gif_u16(&in), ff = gif_byte(&in), mcs;
            const uint32_t *pal = wk->gpal;
            if (ff < 0) return frames;
            if (ff & 0x80)
            {
                if (!gif_read_pal(&in, wk->pal, 2 << (ff & 7))) return frames;
                pal = wk->pal;
            }
            mcs = gif_byte(&in);
            if (mcs < 1 || mcs > 11) return frames ? frames : -2;
            if (count_only)
            {
                if (!gif_skip_blocks(&in, wk->blk)) return frames;
                frames++;
                disposal = 0; transp = -1; delay = 0;
                continue;
            }
            if (disposal == 3) memcpy(prev, canvas, (size_t)W * H * 4);
            {   /* LZW */
                int clear = 1 << mcs, eoi = clear + 1, avail = clear + 2, csize = mcs + 1, cmask = (1 << csize) - 1;
                int old = -1, first = 0, sp = 0, bits = 0, bn = 0, bp = 0, px = 0, total = fw * fh, row = 0, col = 0, pass = 0;
                uint32_t acc = 0;
                bool done = false;
                for (int i = 0; i < clear; i++) { wk->prefix[i] = 0xFFFF; wk->suffix[i] = (uint8_t)i; }
                while (!done)
                {
                    int code, incode;
                    while (bits < csize)
                    {
                        if (bp >= bn)
                        {
                            bn = gif_byte(&in);
                            if (bn <= 0) { done = true; break; }
                            if (rd(rctx, wk->blk, bn) != bn) { in.eof = true; done = true; break; }
                            bp = 0;
                        }
                        acc |= (uint32_t)wk->blk[bp++] << bits; bits += 8;
                    }
                    if (done) break;
                    code = (int)(acc & (uint32_t)cmask); acc >>= csize; bits -= csize;
                    if (code == clear) { csize = mcs + 1; cmask = (1 << csize) - 1; avail = clear + 2; old = -1; continue; }
                    if (code == eoi) break;
                    if (old < 0)
                    {
                        if (code >= clear) break;
                        wk->stack[sp++] = wk->suffix[code]; old = code; first = code;
                    }
                    else
                    {
                        incode = code;
                        if (code > avail) break;                    /* broken stream */
                        if (code == avail) { wk->stack[sp++] = (uint8_t)first; code = old; }
                        while (code >= clear && sp < 4096) { wk->stack[sp++] = wk->suffix[code]; code = wk->prefix[code]; }
                        first = wk->suffix[code];
                        wk->stack[sp++] = (uint8_t)first;
                        if (avail < 4096)
                        {
                            wk->prefix[avail] = (uint16_t)old; wk->suffix[avail] = (uint8_t)first; avail++;
                            if ((avail & cmask) == 0 && avail < 4096) { csize++; cmask = (1 << csize) - 1; }
                        }
                        old = incode;
                    }
                    while (sp > 0)
                    {   /* the stack holds the string reversed */
                        int idx = wk->stack[--sp], x, y;
                        if (px >= total) continue;
                        x = fx + col; y = fy + row;
                        if (idx != transp && x < W && y < H) canvas[y * W + x] = pal[idx];
                        px++;
                        if (++col >= fw)
                        {
                            col = 0;
                            if (ff & 0x40)
                            {   /* interlaced: rows 0,8,.. then 4,12,.. then 2,6,.. then 1,3,.. */
                                static const uint8_t st[4] = { 0, 4, 2, 1 }, inc[4] = { 8, 8, 4, 2 };
                                row += inc[pass];
                                while (row >= fh && pass < 3) { pass++; row = st[pass]; }
                            }
                            else row++;
                        }
                    }
                }
                if (!done) gif_skip_blocks(&in, wk->blk);       /* the rest of the data (and its terminator) */
            }
            if (fr && !fr(fctx, canvas, W, H, delay, frames)) return frames + 1;
            frames++;
            if (disposal == 2)
            {   /* back to the background: transparent (as browsers do) */
                for (int y = fy; y < fy + fh && y < H; y++)
                    for (int x = fx; x < fx + fw && x < W; x++) canvas[y * W + x] = 0;
            }
            else if (disposal == 3) memcpy(canvas, prev, (size_t)W * H * 4);
            disposal = 0; transp = -1; delay = 0;
        }
    }
}
#endif
