/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * bcm overlay v73: soft subtitles (see bcm_subs.h)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#include "bcm_subs.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

#ifdef SUBS_HOST_TEST
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#else
#include "config.h"
#include "file.h"
#include "dir.h"
#include "string-extra.h"
#endif

/* ======================================================================
   UTF-8 helpers (self-contained: also built on the PC for the tests) */

static const uint8_t *u8_next(const uint8_t *p, const uint8_t *e, uint32_t *ucs)
{
    uint32_t c = *p++, n = 0;
    if (c < 0x80) { *ucs = c; return p; }
    if (c >= 0xf0 && c < 0xf8) { c &= 7; n = 3; }
    else if (c >= 0xe0) { c &= 15; n = 2; }
    else if (c >= 0xc0) { c &= 31; n = 1; }
    else { *ucs = 0xfffd; return p; }
    while (n--)
    {
        if (p >= e || (*p & 0xc0) != 0x80) { *ucs = 0xfffd; return p; }
        c = (c << 6) | (*p++ & 0x3f);
    }
    *ucs = c;
    return p;
}

static int u8_put(uint8_t *o, uint32_t c)
{
    if (c < 0x80) { o[0] = (uint8_t)c; return 1; }
    if (c < 0x800) { o[0] = 0xc0 | (c >> 6); o[1] = 0x80 | (c & 63); return 2; }
    if (c < 0x10000) { o[0] = 0xe0 | (c >> 12); o[1] = 0x80 | ((c >> 6) & 63); o[2] = 0x80 | (c & 63); return 3; }
    o[0] = 0xf0 | (c >> 18); o[1] = 0x80 | ((c >> 12) & 63); o[2] = 0x80 | ((c >> 6) & 63); o[3] = 0x80 | (c & 63);
    return 4;
}

static bool u8_valid(const uint8_t *p, uint32_t n)
{
    const uint8_t *e = p + n;
    while (p < e)
    {
        uint32_t c = *p++, k;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c < 0xe0) k = 1;
        else if (c >= 0xe0 && c < 0xf0) k = 2;
        else if (c >= 0xf0 && c < 0xf5) k = 3;
        else return false;
        while (k--) if (p >= e || (*p++ & 0xc0) != 0x80) return false;
    }
    return true;
}

/* Windows-1252 0x80-0x9f (the rest of 0xa0-0xff is Latin-1) */
static const uint16_t cp1252[32] = {
    0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd,
    0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178 };

enum { ENC_UTF8, ENC_UTF16LE, ENC_UTF16BE, ENC_1252 };

static int detect_enc(const uint8_t *p, uint32_t n, uint32_t *skip)
{
    *skip = 0;
    if (n >= 3 && p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) { *skip = 3; return ENC_UTF8; }
    if (n >= 2 && p[0] == 0xff && p[1] == 0xfe) { *skip = 2; return ENC_UTF16LE; }
    if (n >= 2 && p[0] == 0xfe && p[1] == 0xff) { *skip = 2; return ENC_UTF16BE; }
    if (n >= 4 && p[1] == 0 && p[3] == 0 && p[0] && p[2]) return ENC_UTF16LE;     /* no BOM, ASCII text */
    if (n >= 4 && p[0] == 0 && p[2] == 0 && p[1] && p[3]) return ENC_UTF16BE;
    return u8_valid(p, n) ? ENC_UTF8 : ENC_1252;
}

/* converted length (pass 1) or conversion into 'o' (pass 2) */
static uint32_t to_utf8(const uint8_t *p, uint32_t n, int enc, uint8_t *o)
{
    uint32_t i = 0, k = 0;
    uint8_t tmp[4];
    while (i < n)
    {
        uint32_t c;
        if (enc == ENC_1252) { c = p[i++]; if (c >= 0x80 && c < 0xa0) c = cp1252[c - 0x80]; }
        else
        {
            if (i + 1 >= n) break;
            c = enc == ENC_UTF16LE ? (uint32_t)(p[i] | p[i + 1] << 8) : (uint32_t)(p[i] << 8 | p[i + 1]);
            i += 2;
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < n)
            {
                uint32_t d = enc == ENC_UTF16LE ? (uint32_t)(p[i] | p[i + 1] << 8) : (uint32_t)(p[i] << 8 | p[i + 1]);
                if (d >= 0xdc00 && d < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); i += 2; }
            }
        }
        k += u8_put(o ? o + k : tmp, c);
    }
    return k;
}

/* ======================================================================
   the store */

void subs_store_init(struct subs_store *s, void *buf, uint32_t size)
{
    memset(s, 0, sizeof *s);
    s->buf = buf;
    s->size = size & ~3u;
}

static uint32_t rec_size(uint32_t len) { return (sizeof (struct subs_cue) + len + 1 + 3) & ~3u; }

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool parse_color(const char *v, uint32_t n, uint32_t *rgb)
{
    static const struct { const char *name; uint32_t rgb; } names[] = {
        { "white", 0xffffff }, { "yellow", 0xffff00 }, { "red", 0xff0000 }, { "green", 0x00ff00 },
        { "lime", 0x00ff00 }, { "blue", 0x4060ff }, { "cyan", 0x00ffff }, { "aqua", 0x00ffff },
        { "magenta", 0xff00ff }, { "fuchsia", 0xff00ff }, { "orange", 0xffa500 }, { "silver", 0xc0c0c0 },
        { "gray", 0x808080 }, { "grey", 0x808080 }, { "black", 0x202020 } };
    uint32_t i, v6 = 0;
    while (n && (*v == '"' || *v == '\'' || *v == ' ')) { v++; n--; }
    while (n && (v[n - 1] == '"' || v[n - 1] == '\'' || v[n - 1] == ' ')) n--;
    if (n && *v == '#') { v++; n--; }
    if (n == 6)
    {
        for (i = 0; i < 6 && hexv(v[i]) >= 0; i++) v6 = v6 << 4 | (uint32_t)hexv(v[i]);
        if (i == 6) { *rgb = v6; return true; }
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i].name) == n && !strncasecmp(names[i].name, v, n)) { *rgb = names[i].rgb; return true; }
    return false;
}

static int put_color(uint8_t *o, uint32_t rgb)
{
    o[0] = SUBS_C_COLOR;
    o[1] = (uint8_t)(rgb >> 16) ? (uint8_t)(rgb >> 16) : 1;
    o[2] = (uint8_t)(rgb >> 8) ? (uint8_t)(rgb >> 8) : 1;
    o[3] = (uint8_t)rgb ? (uint8_t)rgb : 1;
    return 4;
}

/* SRT / WebVTT / tx3g text -> stored form.  Returns the length; *flags gets SUBS_TOP for {\an7-9} */
static uint32_t norm_text(const char *t, uint32_t n, uint8_t *o, uint16_t *flags)
{
    uint32_t i = 0, k = 0;
    bool line_start = true;
    while (i < n)
    {
        char c = t[i];
        if (c == '\r') { i++; continue; }
        if (c == '<')
        {   /* <i> </i> <b> <u> <font color=..> </font> <c.class> <v Name> <00:00.000> ... */
            uint32_t j = i + 1;
            while (j < n && t[j] != '>' && t[j] != '\n') j++;
            if (j < n && t[j] == '>')
            {
                const char *tag = t + i + 1;
                uint32_t tl = j - i - 1;
                if (tl == 1 && (tag[0] | 0x20) == 'i') o[k++] = SUBS_C_ITALIC_ON;
                else if (tl == 2 && tag[0] == '/' && (tag[1] | 0x20) == 'i') o[k++] = SUBS_C_ITALIC_OFF;
                else if (tl > 5 && !strncasecmp(tag, "font ", 5))
                {
                    const char *cp = NULL;
                    uint32_t q;
                    for (q = 5; q + 6 <= tl; q++) if (!strncasecmp(tag + q, "color=", 6)) { cp = tag + q + 6; break; }
                    if (cp)
                    {
                        uint32_t vl = 0, rgb;
                        while (cp + vl < tag + tl && cp[vl] != ' ') vl++;
                        if (parse_color(cp, vl, &rgb)) k += put_color(o + k, rgb);
                    }
                }
                else if (tl == 5 && !strncasecmp(tag, "/font", 5)) o[k++] = SUBS_C_COLOR_OFF;
                i = j + 1;
                continue;
            }
        }
        if (c == '{' && i + 1 < n && t[i + 1] == '\\')
        {   /* ASS-style tags inside SRT: {\an8} {\i1} ... */
            uint32_t j = i + 1;
            while (j < n && t[j] != '}') j++;
            if (j < n)
            {
                const char *g = t + i + 1;
                uint32_t gl = j - i - 1, q;
                for (q = 0; q + 3 < gl; q++)
                    if (g[q] == '\\' && g[q + 1] == 'a' && g[q + 2] == 'n' && g[q + 3] >= '7' && g[q + 3] <= '9') *flags |= SUBS_TOP;
                for (q = 0; q + 2 < gl; q++)
                    if (g[q] == '\\' && g[q + 1] == 'i' && (g[q + 2] == '1' || g[q + 2] == '0'))
                        o[k++] = g[q + 2] == '1' ? SUBS_C_ITALIC_ON : SUBS_C_ITALIC_OFF;
                i = j + 1;
                continue;
            }
        }
        if (c == '&')
        {
            static const struct { const char *e; uint32_t c; } ent[] = {
                { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&nbsp;", 0xa0 }, { "&quot;", '"' }, { "&apos;", '\'' }, { "&lrm;", 0 }, { "&rlm;", 0 } };
            uint32_t q;
            for (q = 0; q < sizeof ent / sizeof ent[0]; q++)
            {
                uint32_t el = strlen(ent[q].e);
                if (i + el <= n && !strncmp(t + i, ent[q].e, el))
                {
                    if (ent[q].c) k += u8_put(o + k, ent[q].c);
                    i += el;
                    break;
                }
            }
            if (q < sizeof ent / sizeof ent[0]) continue;
        }
        if (c == '\n')
        {   /* one line break, no empty lines, no trailing spaces */
            while (k && o[k - 1] == ' ') k--;
            if (k && o[k - 1] != '\n') o[k++] = '\n';
            line_start = true;
            i++;
            continue;
        }
        if (c == '\t') c = ' ';
        if (c == ' ' && (line_start || (k && o[k - 1] == ' '))) { i++; continue; }
        o[k++] = (uint8_t)c;
        line_start = false;
        i++;
    }
    while (k && (o[k - 1] == '\n' || o[k - 1] == ' ')) k--;
    return k;
}

static bool add_norm(struct subs_store *s, uint32_t start, uint32_t end, const uint8_t *txt, uint32_t len, uint16_t flags)
{
    struct subs_cue *c;
    bool visible = false;
    uint32_t i;
    for (i = 0; i < len; i++) if (txt[i] > ' ' || txt[i] >= 0x80) { visible = true; break; }
    if (!visible || end <= start) return true;               /* nothing to show: not an error */
    if (len > 0xfff0) len = 0xfff0;
    if (s->used + rec_size(len) > s->size) return false;
    c = (struct subs_cue *)(s->buf + s->used);
    c->start = start; c->end = end; c->len = (uint16_t)len; c->flags = flags;
    memmove((uint8_t *)(c + 1), txt, len);
    ((uint8_t *)(c + 1))[len] = 0;
    s->used += rec_size(len);
    s->n++;
    if (end - start > s->maxdur) s->maxdur = end - start;
    return true;
}

bool subs_add(struct subs_store *s, uint32_t start, uint32_t end, const char *text, uint32_t len, uint16_t flags)
{
    struct subs_cue *c;
    uint32_t n;
    if (s->used + rec_size(len + len / 2 + 8) > s->size) return false;
    c = (struct subs_cue *)(s->buf + s->used);
    n = norm_text(text, len, (uint8_t *)(c + 1), &flags);
    return add_norm(s, start, end, (const uint8_t *)(c + 1), n, flags);
}

static const uint8_t *sort_base;
static int cmp_cue(const void *a, const void *b)
{
    const struct subs_cue *x = (const struct subs_cue *)(sort_base + *(const uint32_t *)a);
    const struct subs_cue *y = (const struct subs_cue *)(sort_base + *(const uint32_t *)b);
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return *(const uint32_t *)a < *(const uint32_t *)b ? -1 : 1;     /* stable: file order */
}

void subs_finish(struct subs_store *s)
{
    uint32_t off = 0, i = 0, need = (s->used + 3) & ~3u;
    bool sorted = true;
    uint32_t last = 0;
    if (need + 4 * s->n > s->size)
    {   /* no room for the index: drop cues from the end until it fits */
        while (s->n && need + 4 * s->n > s->size)
        {
            uint32_t o = 0, prev = 0, k;
            for (k = 0; k + 1 < s->n; k++) { prev = o; o += rec_size(((struct subs_cue *)(s->buf + o))->len); }
            (void)prev;
            s->used = o; s->n--; need = (s->used + 3) & ~3u;
        }
    }
    s->idx = (uint32_t *)(s->buf + need);
    while (off < s->used && i < s->n)
    {
        const struct subs_cue *c = (const struct subs_cue *)(s->buf + off);
        if (c->start < last) sorted = false;
        last = c->start;
        s->idx[i++] = off;
        off += rec_size(c->len);
    }
    s->n = i;
    if (!sorted) { sort_base = s->buf; qsort(s->idx, s->n, sizeof (uint32_t), cmp_cue); }
}

const struct subs_cue *subs_cue_at(const struct subs_store *s, uint32_t i)
{
    return i < s->n ? (const struct subs_cue *)(s->buf + s->idx[i]) : NULL;
}

int subs_active(const struct subs_store *s, uint32_t ms, const struct subs_cue **out, int max)
{
    uint32_t lo = 0, hi = s->n, i;
    int n = 0;
    if (!s->n || !s->idx) return 0;
    while (lo < hi)                                   /* first cue starting after ms */
    {
        uint32_t mid = (lo + hi) / 2;
        if (subs_cue_at(s, mid)->start <= ms) lo = mid + 1; else hi = mid;
    }
    for (i = lo; i-- > 0; )
    {   /* walk back while a cue could still be running */
        const struct subs_cue *c = subs_cue_at(s, i);
        if (ms - c->start > s->maxdur) break;
        if (c->end > ms)
        {
            if (n == max) { memmove(out, out + 1, sizeof *out * (max - 1)); n--; }   /* keep the latest */
            out[n++] = c;
        }
    }
    for (i = 0; i < (uint32_t)n / 2; i++) { const struct subs_cue *t = out[i]; out[i] = out[n - 1 - i]; out[n - 1 - i] = t; }
    return n;
}

/* ======================================================================
   parsers */

/* "01:02:03,456" "01:02:03.456" "02:03.456" "0:01:02.34" -> ms; returns chars used, 0 = no time */
static int parse_time(const char *p, const char *e, uint32_t *ms)
{
    uint32_t f[3] = { 0, 0, 0 }, nf = 0, frac = 0, fd = 0, v;
    const char *s = p;
    while (p < e && *p == ' ') p++;
    for (;;)
    {
        if (p >= e || !isdigit((unsigned char)*p)) return 0;
        v = 0;
        while (p < e && isdigit((unsigned char)*p)) v = v * 10 + (uint32_t)(*p++ - '0');
        if (nf == 3) return 0;
        f[nf++] = v;
        if (p < e && *p == ':') { p++; continue; }
        break;
    }
    if (p < e && (*p == ',' || *p == '.'))
    {
        p++;
        while (p < e && isdigit((unsigned char)*p)) { if (fd < 3) { frac = frac * 10 + (uint32_t)(*p - '0'); fd++; } p++; }
        while (fd && fd < 3) { frac *= 10; fd++; }
    }
    if (nf < 2) return 0;
    if (nf == 2) { f[2] = f[1]; f[1] = f[0]; f[0] = 0; }
    *ms = ((f[0] * 60 + f[1]) * 60 + f[2]) * 1000 + frac;
    return (int)(p - s);
}

static const char *line_end(const char *p, const char *e) { while (p < e && *p != '\n') p++; return p; }
static bool blank_line(const char *p, const char *le) { while (p < le && (*p == ' ' || *p == '\r' || *p == '\t')) p++; return p == le; }

/* a timing line "A --> B [settings]" */
static bool timing_line(const char *p, const char *le, uint32_t *a, uint32_t *b, uint16_t *flags)
{
    const char *arrow = p;
    int k;
    while (arrow + 3 <= le && strncmp(arrow, "-->", 3)) arrow++;
    if (arrow + 3 > le) return false;
    if (!(k = parse_time(p, arrow, a))) return false;
    if (!(k = parse_time(arrow + 3, le, b))) return false;
    {   /* WebVTT settings: line:0% / line:0 -> top */
        const char *q = arrow + 3 + k;
        for (; q + 5 < le; q++)
            if (!strncmp(q, "line:", 5))
            {
                const char *r = q + 5;
                bool neg = *r == '-';
                uint32_t v = 0;
                if (neg) r++;
                while (r < le && isdigit((unsigned char)*r)) v = v * 10 + (uint32_t)(*r++ - '0');
                if (!neg && ((r < le && *r == '%') ? v < 40 : v < 4)) *flags |= SUBS_TOP;
            }
    }
    return true;
}

static int parse_srt_vtt(struct subs_store *s, const char *p, const char *e, bool vtt)
{
    int cues = 0;
    while (p < e)
    {
        const char *le = line_end(p, e);
        uint32_t a, b;
        uint16_t flags = 0;
        if (vtt && le - p >= 4 && (!strncmp(p, "NOTE", 4) || !strncmp(p, "STYLE", 5) || !strncmp(p, "REGION", 6)))
        {   /* skip the block */
            while (p < e) { le = line_end(p, e); if (blank_line(p, le)) break; p = le < e ? le + 1 : e; }
            continue;
        }
        if (timing_line(p, le, &a, &b, &flags))
        {
            const char *ts = le < e ? le + 1 : e, *te = ts, *q = ts;
            /* text: until a blank line, or until a line that is a cue number followed by a timing line */
            while (q < e)
            {
                const char *qe = line_end(q, e), *n2;
                uint32_t x, y;
                uint16_t fl = 0;
                if (blank_line(q, qe)) break;
                if (timing_line(q, qe, &x, &y, &fl)) break;
                n2 = qe < e ? qe + 1 : e;
                if (n2 < e)
                {   /* "123" then a timing line: the next cue (missing blank line) */
                    const char *r = q;
                    while (r < qe && (isdigit((unsigned char)*r) || *r == '\r' || *r == ' ')) r++;
                    if (r == qe && timing_line(n2, line_end(n2, e), &x, &y, &fl)) break;
                }
                te = qe;
                q = n2;
            }
            {   /* records are written from the buffer start, behind the input: a record (12-byte header +
                   normalised text) is never longer than the timing line + text it came from */
                struct subs_cue *c = (struct subs_cue *)(s->buf + s->used);
                uint32_t n = norm_text(ts, (uint32_t)(te - ts), (uint8_t *)(c + 1), &flags);
                if (!add_norm(s, a, b, (const uint8_t *)(c + 1), n, flags)) return cues;
                cues++;
            }
            p = q;
            continue;
        }
        p = le < e ? le + 1 : e;
    }
    return cues;
}

/* ASS / SSA: [Events] Format: + Dialogue: lines */
static uint32_t ass_text(const char *t, uint32_t n, uint8_t *o, uint16_t *flags)
{
    uint32_t i = 0, k = 0;
    bool drawing = false;
    while (i < n)
    {
        char c = t[i];
        if (c == '{')
        {
            uint32_t j = i + 1;
            while (j < n && t[j] != '}') j++;
            {
                const char *g = t + i + 1;
                uint32_t gl = j - i - 1, q;
                for (q = 0; q + 1 < gl; q++)
                {
                    if (g[q] != '\\') continue;
                    if (g[q + 1] == 'a' && q + 3 < gl + 1 && g[q + 2] == 'n' && q + 3 < gl && g[q + 3] >= '7' && g[q + 3] <= '9') *flags |= SUBS_TOP;
                    else if (g[q + 1] == 'a' && q + 2 < gl && isdigit((unsigned char)g[q + 2]))
                    {   /* legacy \a: 5-7 = top, 9-11 = middle (drawn at the bottom) */
                        int v = g[q + 2] - '0';
                        if (q + 3 < gl && isdigit((unsigned char)g[q + 3])) v = v * 10 + g[q + 3] - '0';
                        if (v >= 5 && v <= 7) *flags |= SUBS_TOP;
                    }
                    else if (g[q + 1] == 'i' && q + 2 < gl && (g[q + 2] == '1' || g[q + 2] == '0'))
                        o[k++] = g[q + 2] == '1' ? SUBS_C_ITALIC_ON : SUBS_C_ITALIC_OFF;
                    else if (g[q + 1] == 'p' && q + 2 < gl && isdigit((unsigned char)g[q + 2]))
                        drawing = g[q + 2] != '0';
                    else if ((g[q + 1] == 'c' || (g[q + 1] == '1' && q + 2 < gl && g[q + 2] == 'c')) )
                    {   /* \c&HBBGGRR& or \1c&HBBGGRR& */
                        uint32_t r = q + (g[q + 1] == 'c' ? 2 : 3), v = 0, d = 0;
                        if (r + 1 < gl && g[r] == '&' && (g[r + 1] == 'H' || g[r + 1] == 'h'))
                        {
                            r += 2;
                            while (r < gl && hexv(g[r]) >= 0) { v = v << 4 | (uint32_t)hexv(g[r]); r++; d++; }
                            if (d) k += put_color(o + k, ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff));
                        }
                        else if (r >= gl || g[r] == '\\') o[k++] = SUBS_C_COLOR_OFF;   /* \c alone: back to the style colour */
                    }
                    else if (g[q + 1] == 'r') { o[k++] = SUBS_C_ITALIC_OFF; o[k++] = SUBS_C_COLOR_OFF; }
                }
            }
            i = j < n ? j + 1 : n;
            continue;
        }
        if (drawing) { i++; continue; }
        if (c == '\\' && i + 1 < n && (t[i + 1] == 'N' || t[i + 1] == 'n' || t[i + 1] == 'h'))
        {
            if (t[i + 1] == 'N') { while (k && o[k - 1] == ' ') k--; if (k && o[k - 1] != '\n') o[k++] = '\n'; }
            else if (k && o[k - 1] != ' ' && o[k - 1] != '\n') o[k++] = ' ';
            i += 2;
            continue;
        }
        if (c == '\r' || c == '\n') { i++; continue; }
        if (c == ' ' && (!k || o[k - 1] == ' ' || o[k - 1] == '\n')) { i++; continue; }
        o[k++] = (uint8_t)c;
        i++;
    }
    while (k && (o[k - 1] == '\n' || o[k - 1] == ' ')) k--;
    return k;
}

static int parse_ass(struct subs_store *s, const char *p, const char *e)
{
    int cues = 0, f_start = 1, f_end = 2, f_text = 9, nfields = 10;
    bool events = false;
    while (p < e)
    {
        const char *le = line_end(p, e), *x = p;
        while (x < le && (*x == ' ' || *x == '\t')) x++;
        if (*x == '[') events = (le - x >= 8 && !strncasecmp(x, "[Events]", 8));
        else if (events && le - x > 7 && !strncasecmp(x, "Format:", 7))
        {   /* field order */
            const char *q = x + 7;
            int f = 0;
            while (q < le)
            {
                const char *fe = q;
                while (fe < le && *fe != ',') fe++;
                while (q < fe && *q == ' ') q++;
                if (fe - q >= 5 && !strncasecmp(q, "Start", 5)) f_start = f;
                else if (fe - q >= 3 && !strncasecmp(q, "End", 3)) f_end = f;
                else if (fe - q >= 4 && !strncasecmp(q, "Text", 4)) f_text = f;
                f++;
                q = fe < le ? fe + 1 : le;
            }
            nfields = f;
            if (f_text != nfields - 1) f_text = nfields - 1;
        }
        else if (events && le - x > 9 && !strncasecmp(x, "Dialogue:", 9))
        {
            const char *q = x + 9, *fs[16], *fe[16];
            int f = 0;
            uint32_t a = 0, b = 0;
            while (q <= le && f < 16)
            {
                const char *z = q;
                if (f == nfields - 1) z = le;                        /* the text: the rest, commas and all */
                else while (z < le && *z != ',') z++;
                fs[f] = q; fe[f] = z; f++;
                if (z >= le) break;
                q = z + 1;
            }
            if (f >= nfields && f_start < f && f_end < f && f_text < f &&
                parse_time(fs[f_start], fe[f_start], &a) && parse_time(fs[f_end], fe[f_end], &b))
            {
                uint16_t flags = 0;
                struct subs_cue *c = (struct subs_cue *)(s->buf + s->used);
                uint32_t len = (uint32_t)(fe[f_text] - fs[f_text]), n;
                n = ass_text(fs[f_text], len, (uint8_t *)(c + 1), &flags);
                if (!add_norm(s, a, b, (const uint8_t *)(c + 1), n, flags)) return cues;
                cues++;
            }
        }
        p = le < e ? le + 1 : e;
    }
    return cues;
}

uint32_t subs_buffer_size(uint32_t file_size)
{   /* raw file + converted text (up to 3x for Windows-1252) + records + index */
    return file_size * 4 + 65536;
}

/* the raw file is at the top of the buffer: convert it (if needed) just below itself, then parse;
   records are written from the buffer start and never catch up with the text being read */
static int parse_top(struct subs_store *s, uint32_t len, int kind)
{
    uint8_t *raw = s->buf + s->size - len;
    uint32_t skip, n;
    int enc = detect_enc(raw, len, &skip), r;
    const char *p;
    s->used = 0; s->n = 0; s->idx = NULL; s->maxdur = 0;
    if (enc == ENC_UTF8) { p = (const char *)raw + skip; n = len - skip; }
    else
    {
        uint8_t *dst;
        n = to_utf8(raw + skip, len - skip, enc, NULL);
        if (len + n + 1024 > s->size) return -2;
        dst = raw - n;
        to_utf8(raw + skip, len - skip, enc, dst);
        p = (const char *)dst;
    }
    if (kind == SUBS_ASS) r = parse_ass(s, p, p + n);
    else r = parse_srt_vtt(s, p, p + n, kind == SUBS_VTT || (n >= 6 && !strncmp(p, "WEBVTT", 6)));
    if (r < 0) return r;
    subs_finish(s);
    return (int)s->n;
}

int subs_parse(struct subs_store *s, const uint8_t *raw, uint32_t len, int kind)
{
    if (len + 1024 > s->size) return -2;
    memmove(s->buf + s->size - len, raw, len);
    return parse_top(s, len, kind);
}

int subs_load_file(struct subs_store *s, const char *path, int kind)
{
    int fd = open(path, O_RDONLY), r;
    uint32_t sz;
    if (fd < 0) return -1;
    sz = (uint32_t)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (!sz || sz + 1024 > s->size) { close(fd); return -2; }
    r = (int)read(fd, s->buf + s->size - sz, sz);
    close(fd);
    if (r != (int)sz) return -3;
    return parse_top(s, sz, kind);
}

/* ======================================================================
   finding sidecar files */

static int kind_of_ext(const char *e)
{
    if (!strcasecmp(e, "srt")) return SUBS_SRT;
    if (!strcasecmp(e, "vtt")) return SUBS_VTT;
    if (!strcasecmp(e, "ass") || !strcasecmp(e, "ssa")) return SUBS_ASS;
    return SUBS_NONE;
}

static int lang_rank(const char *mid)
{   /* lower = better: none, English, other */
    static const char *const en[] = { "en", "eng", "english", "en-us", "en-gb", "en_us", "en_gb" };
    char first[16];
    unsigned i = 0;
    if (!*mid) return 0;
    bool forced = strcasestr(mid, "forced") != NULL;
    while (mid[i] && mid[i] != '.' && mid[i] != ' ' && i < sizeof first - 1) { first[i] = mid[i]; i++; }
    first[i] = 0;
    for (i = 0; i < sizeof en / sizeof en[0]; i++) if (!strcasecmp(first, en[i])) return forced ? 3 : 1;
    return forced ? 4 : 2;
}

int subs_find_sidecars(const char *video, struct subs_track *t, int max)
{
    char dir[SUBS_PATH_MAX], base[SUBS_PATH_MAX];
    const char *slash = strrchr(video, '/'), *dot;
    size_t bl;
    int n = 0, i, j;
    DIR *d;
    struct dirent *de;
    if (!slash || (size_t)(slash - video) >= sizeof dir) return 0;
    memcpy(dir, video, slash - video); dir[slash - video] = 0;
    if (!dir[0]) strcpy(dir, "/");
    snprintf(base, sizeof base, "%s", slash + 1);
    dot = strrchr(base, '.');
    if (dot) base[dot - base] = 0;
    bl = strlen(base);
    d = opendir(dir);
    if (!d) return 0;
    while ((de = readdir(d)) != NULL && n < max)
    {
        const char *nm = de->d_name, *ext = strrchr(nm, '.');
        int kind;
        size_t ml;
        if (!ext || strncasecmp(nm, base, bl) || (nm[bl] != '.')) continue;
        kind = kind_of_ext(ext + 1);
        if (!kind) continue;
        t[n].kind = (uint8_t)kind;
        t[n].index = 0;
        snprintf(t[n].path, sizeof t[n].path, "%s/%s", strcmp(dir, "/") ? dir : "", nm);
        ml = (size_t)(ext - (nm + bl));                    /* ".en.forced" part, with its leading dot */
        if (ml > 1)
        {
            size_t k;
            ml--;
            if (ml >= sizeof t[n].label) ml = sizeof t[n].label - 1;
            memcpy(t[n].label, nm + bl + 1, ml); t[n].label[ml] = 0;
            for (k = 0; k < ml; k++) if (t[n].label[k] == '.' || t[n].label[k] == '_') t[n].label[k] = ' ';
        }
        else t[n].label[0] = 0;
        n++;
    }
    closedir(d);
    /* best first: no language part, then English, then the rest (forced after full), then by name, then format */
    for (i = 1; i < n; i++)
        for (j = i; j > 0; j--)
        {
            const char *ma = t[j - 1].label, *mb = t[j].label;
            int ra = lang_rank(ma), rb = lang_rank(mb);
            int c = strcasecmp(ma, mb);
            /* same language: SRT before WebVTT before ASS (ASS styling is only partly kept) */
            if (ra < rb || (ra == rb && (c < 0 || (c == 0 && t[j - 1].kind <= t[j].kind)))) break;
            { struct subs_track x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
        }
    for (i = 0; i < n; i++)
        if (!t[i].label[0])
            snprintf(t[i].label, sizeof t[i].label, "%s", t[i].kind == SUBS_SRT ? "SRT" : t[i].kind == SUBS_VTT ? "WebVTT" : "ASS");
    return n;
}

/* ======================================================================
   drawing */

#define MAXLINES 12

struct run { const uint8_t *p, *e; bool italic; uint32_t color; };

static uint32_t glyph_w(const struct subs_font *f, uint32_t ucs) { return (uint32_t)f->width(f->ctx, ucs); }

/* width of [p,e) in font pixels, style codes skipped */
static int text_w(const struct subs_font *f, const uint8_t *p, const uint8_t *e)
{
    int w = 0;
    while (p < e)
    {
        uint32_t c;
        if (*p == SUBS_C_COLOR) { p += 4; continue; }
        if (*p < 0x05) { p++; continue; }
        p = u8_next(p, e, &c);
        w += (int)glyph_w(f, c);
    }
    return w;
}

/* plot one glyph into the alpha mask (and its colour into the canvas) */
static void blit_glyph(uint32_t *cv, uint8_t *m, int w, int h, const struct subs_font *f, int scale,
                       uint32_t ucs, int x, int y, bool italic, uint32_t rgb)
{
    const uint8_t *b = f->bits(f->ctx, ucs);
    int gw = f->width(f->ctx, ucs), r, c, sx, sy;
    if (!b || gw <= 0) return;
    for (r = 0; r < f->height; r++)
    {
        int sh = italic ? (f->height - r) * scale / 5 - f->height * scale / 10 : 0;
        for (c = 0; c < gw; c++)
        {
            unsigned a;
            if (f->depth)
            {   /* 4-bit alpha, rows, 2 pixels a byte, 0 = opaque */
                unsigned i = (unsigned)(r * gw + c), v = b[i >> 1];
                v = (i & 1) ? v >> 4 : v & 15;
                a = (15 - v) * 17;
            }
            else a = (b[(r >> 3) * gw + c] >> (r & 7)) & 1 ? 255 : 0;
            if (!a) continue;
            for (sy = 0; sy < scale; sy++)
                for (sx = 0; sx < scale; sx++)
                {
                    int px = x + c * scale + sx + sh, py = y + r * scale + sy;
                    if (px < 0 || py < 0 || px >= w || py >= h) continue;
                    if (a > m[py * w + px]) { m[py * w + px] = (uint8_t)a; cv[py * w + px] = rgb; }
                }
        }
    }
}

/* break the cue text into lines that fit 'maxw' font pixels */
struct vline { const uint8_t *p, *e; bool italic; uint32_t color; int w; };

static int wrap(const struct subs_font *f, const uint8_t *p, const uint8_t *e, int maxw, struct vline *out, int max,
                bool *italic, uint32_t *color)
{
    int n = 0;
    while (p < e && n < max)
    {
        const uint8_t *ls = p, *le = p, *best = NULL;
        bool it0 = *italic; uint32_t col0 = *color;
        int w = 0;
        /* hard line: up to '\n' */
        while (le < e && *le != '\n') le++;
        /* fit words */
        {
            const uint8_t *q = ls;
            while (q < le)
            {
                uint32_t c;
                const uint8_t *q0 = q;
                if (*q == SUBS_C_COLOR) { q += 4; continue; }
                if (*q < 0x05) { q++; continue; }
                q = u8_next(q, le, &c);
                if (c == ' ') best = q0;
                w += (int)glyph_w(f, c);
                if (w > maxw && best && best > ls) { le = best; break; }
            }
        }
        out[n].p = ls; out[n].e = le; out[n].italic = it0; out[n].color = col0;
        out[n].w = text_w(f, ls, le);
        n++;
        {   /* carry the style state past this line */
            const uint8_t *q = ls;
            while (q < le)
            {
                if (*q == SUBS_C_ITALIC_ON) *italic = true;
                else if (*q == SUBS_C_ITALIC_OFF) *italic = false;
                else if (*q == SUBS_C_COLOR && q + 3 < le) { *color = (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]; q += 3; }
                else if (*q == SUBS_C_COLOR_OFF) *color = 0;
                q++;
            }
        }
        p = le;
        while (p < e && (*p == ' ' || *p == '\n')) p++;
    }
    return n;
}

static void draw_line(uint32_t *cv, uint8_t *m, int w, int h, const struct subs_font *f, int scale,
                      const struct vline *l, int x, int y, uint32_t defcol)
{
    const uint8_t *p = l->p;
    bool italic = l->italic;
    uint32_t color = l->color;
    while (p < l->e)
    {
        uint32_t c;
        if (*p == SUBS_C_ITALIC_ON) { italic = true; p++; continue; }
        if (*p == SUBS_C_ITALIC_OFF) { italic = false; p++; continue; }
        if (*p == SUBS_C_COLOR) { if (p + 3 < l->e) color = (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; p += 4; continue; }
        if (*p == SUBS_C_COLOR_OFF) { color = 0; p++; continue; }
        p = u8_next(p, l->e, &c);
        if (c == 0xa0) c = ' ';
        blit_glyph(cv, m, w, h, f, scale, c, x, y, italic, color ? color : defcol);
        x += (int)glyph_w(f, c) * scale;
    }
}

int subs_render(uint32_t *canvas, uint8_t *work, int w, int h, const struct subs_font *f,
                const struct subs_style *st, const struct subs_cue **cues, int n, const char *msg,
                int top, int bottom, int *y0, int *y1)
{
    struct vline lines[MAXLINES];
    int nl = 0, i, lh, pad = st->box ? 3 * st->scale : st->outline, total, y, maxw;
    bool attop = false;
    memset(canvas, 0, (size_t)w * h * 4);
    memset(work, 0, (size_t)w * h);
    *y0 = *y1 = 0;
    maxw = (w - 2 * st->margin - 2 * pad) / st->scale;
    if (maxw < 16) return 0;
    if (msg)
    {
        bool it = false; uint32_t col = 0;
        const uint8_t *p = (const uint8_t *)msg;
        nl = wrap(f, p, p + strlen(msg), maxw, lines, MAXLINES, &it, &col);
    }
    else
    {
        int k;
        attop = n > 0;
        for (k = 0; k < n; k++) if (!(cues[k]->flags & SUBS_TOP)) attop = false;
        for (k = 0; k < n && nl < MAXLINES; k++)
        {
            bool it = false; uint32_t col = 0;
            const uint8_t *p = (const uint8_t *)subs_text(cues[k]);
            if (!attop && (cues[k]->flags & SUBS_TOP)) continue;         /* a sign at the top while dialogue shows: dialogue wins */
            nl += wrap(f, p, p + cues[k]->len, maxw, lines + nl, MAXLINES - nl, &it, &col);
        }
    }
    if (!nl) return 0;
    lh = f->height * st->scale + (st->box ? 2 * pad : 0);
    total = nl * lh;
    while (total > (attop ? h - top : bottom) - pad && nl > 1) { nl--; total -= lh; }   /* too many lines: keep the first ones */
    y = attop ? top + pad : bottom - total - pad;
    if (y < 0) y = 0;
    *y0 = y - pad; *y1 = y + total + pad;
    if (*y0 < 0) *y0 = 0;
    if (*y1 > h) *y1 = h;
    for (i = 0; i < nl; i++, y += lh)
    {
        int lw = lines[i].w * st->scale, x = (w - lw) / 2;
        if (st->box)
        {   /* a dark, slightly transparent box per line */
            int bx0 = x - 2 * pad, bx1 = x + lw + 2 * pad, yy, xx;
            if (bx0 < 0) bx0 = 0;
            if (bx1 > w) bx1 = w;
            for (yy = y; yy < y + lh && yy < h; yy++)
                for (xx = bx0; xx < bx1; xx++) canvas[yy * w + xx] = 0xB0000000u;
            draw_line(canvas, work, w, h, f, st->scale, &lines[i], x, y + pad, st->color);
        }
        else draw_line(canvas, work, w, h, f, st->scale, &lines[i], x, y, st->color);
    }
    {   /* compose: the text colour over a black outline (separable max filter = square dilation);
           box style: the text over its box, with a thin dark edge round the letters */
        int r = st->box ? 1 : st->outline, yy, xx, ya = *y0, yb = *y1;
        static uint8_t rowbuf[1024];
        if (w > (int)sizeof rowbuf) return 0;
        for (yy = ya; yy < yb; yy++)
        {
            uint8_t *mr = work + yy * w;
            uint32_t *cr = canvas + yy * w;
            int lo = yy - r < ya ? ya : yy - r, hi = yy + r >= yb ? yb - 1 : yy + r;
            for (xx = 0; xx < w; xx++)
            {
                int q, mx = 0;
                for (q = lo; q <= hi; q++) if (work[q * w + xx] > mx) mx = work[q * w + xx];
                rowbuf[xx] = (uint8_t)mx;
            }
            for (xx = 0; xx < w; xx++)
            {
                int q, o = 0;
                uint32_t at = mr[xx], a, oa, px = cr[xx];
                for (q = xx - r; q <= xx + r; q++) if (q >= 0 && q < w && rowbuf[q] > o) o = rowbuf[q];
                if (!at)
                {   /* no letter here: outline (black), the box, or nothing */
                    oa = st->box ? ((px >> 24) > (uint32_t)o * 3 / 4 ? (px >> 24) : (uint32_t)o * 3 / 4) : (uint32_t)o;
                    cr[xx] = oa << 24;
                    continue;
                }
                oa = st->box ? 0xB0u : (uint32_t)o;
                a = at + oa * (255 - at) / 255;
                cr[xx] = (a << 24) | ((((px >> 16) & 255) * at / a) << 16) | ((((px >> 8) & 255) * at / a) << 8) | ((px & 255) * at / a);
            }
        }
    }
    return 1;
}
