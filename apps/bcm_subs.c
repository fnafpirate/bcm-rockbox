/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * Subtitle parser and renderer — SRT, WebVTT, ASS/SSA, MP4 tx3g.
 *
 * Copyright (C) 2024 bcm-rockbox contributors
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

#include "bcm_subs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#ifdef IPOD_VIDEO
#include "file.h"
#include "font.h"
#include "lcd.h"
#include "bcm_host.h"
#endif

/* -----------------------------------------------------------------------
 * Internal context
 * --------------------------------------------------------------------- */
struct subs_ctx {
    subs_format_t  fmt;
    subs_cue_t    *cues;
    int            ncues;
    int            cap;
};

/* -----------------------------------------------------------------------
 * Utilities
 * --------------------------------------------------------------------- */

/* Strip HTML/ASS tags from @src into @dst (at most dst_sz-1 bytes) */
static void strip_tags(const char *src, char *dst, size_t dst_sz)
{
    size_t di = 0;
    bool in_tag = false;
    for (const char *p = src; *p && di < dst_sz - 1; p++)
    {
        if (*p == '<' || *p == '{') { in_tag = true; continue; }
        if (*p == '>' || *p == '}') { in_tag = false; continue; }
        if (!in_tag) dst[di++] = *p;
    }
    dst[di] = '\0';
}

/* Parse "HH:MM:SS,mmm" or "HH:MM:SS.mmm" into milliseconds */
static bool parse_timestamp(const char *s, uint32_t *ms_out)
{
    unsigned h, m, sec, ms;
    if (sscanf(s, "%u:%u:%u,%u", &h, &m, &sec, &ms) == 4 ||
        sscanf(s, "%u:%u:%u.%u", &h, &m, &sec, &ms) == 4)
    {
        *ms_out = ((uint32_t)h * 3600u + m * 60u + sec) * 1000u + ms;
        return true;
    }
    /* WebVTT short form MM:SS.mmm */
    if (sscanf(s, "%u:%u.%u", &m, &sec, &ms) == 3)
    {
        *ms_out = (m * 60u + sec) * 1000u + ms;
        return true;
    }
    return false;
}

static subs_ctx_t *ctx_alloc(subs_format_t fmt)
{
    subs_ctx_t *ctx = (subs_ctx_t *)malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->fmt   = fmt;
    ctx->ncues = 0;
    ctx->cap   = 64;
    ctx->cues  = (subs_cue_t *)malloc(sizeof(subs_cue_t) * (size_t)ctx->cap);
    if (!ctx->cues) { free(ctx); return NULL; }
    return ctx;
}

static bool ctx_add(subs_ctx_t *ctx, uint32_t start, uint32_t end,
                    const char *raw_text)
{
    if (ctx->ncues >= ctx->cap)
    {
        int new_cap = ctx->cap * 2;
        if (new_cap > SUBS_MAX_CUES) new_cap = SUBS_MAX_CUES;
        if (ctx->ncues >= new_cap) return false;
        subs_cue_t *p = (subs_cue_t *)realloc(ctx->cues,
                              sizeof(subs_cue_t) * (size_t)new_cap);
        if (!p) return false;
        ctx->cues = p;
        ctx->cap  = new_cap;
    }
    subs_cue_t *c = &ctx->cues[ctx->ncues++];
    c->start_ms = start;
    c->end_ms   = end;
    strip_tags(raw_text, c->text, sizeof(c->text));
    return true;
}

/* -----------------------------------------------------------------------
 * SRT parser
 * --------------------------------------------------------------------- */
static subs_ctx_t *parse_srt(const char *buf, size_t len)
{
    subs_ctx_t *ctx = ctx_alloc(SUBS_FMT_SRT);
    if (!ctx) return NULL;

    const char *p   = buf;
    const char *end = buf + len;

    while (p < end)
    {
        /* Skip sequence number line */
        while (p < end && (*p == '\r' || *p == '\n' || *p == ' '))
            p++;
        if (p >= end) break;

        /* Skip numeric index */
        while (p < end && isdigit((unsigned char)*p)) p++;
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        if (p >= end) break;

        /* Timestamp line */
        char ts_line[80];
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        size_t ts_len = (size_t)(nl - p);
        if (ts_len >= sizeof(ts_line)) ts_len = sizeof(ts_line) - 1;
        memcpy(ts_line, p, ts_len);
        ts_line[ts_len] = '\0';
        p = nl + 1;

        uint32_t start = 0, end_t = 0;
        char *arrow = strstr(ts_line, " --> ");
        if (!arrow) continue;
        *arrow = '\0';
        parse_timestamp(ts_line, &start);
        parse_timestamp(arrow + 5, &end_t);

        /* Text lines until blank line */
        char text[SUBS_MAX_TEXT];
        size_t ti = 0;
        while (p < end)
        {
            const char *lend = memchr(p, '\n', (size_t)(end - p));
            size_t llen = lend ? (size_t)(lend - p) : (size_t)(end - p);
            /* Blank line = end of cue */
            if (llen == 0 || (llen == 1 && p[0] == '\r'))
            {
                p = lend ? lend + 1 : end;
                break;
            }
            if (ti > 0 && ti < sizeof(text) - 1)
                text[ti++] = '\n';
            size_t copy = llen;
            if (copy > 0 && p[copy-1] == '\r') copy--;
            if (ti + copy >= sizeof(text)) copy = sizeof(text) - ti - 1;
            memcpy(text + ti, p, copy);
            ti += copy;
            p = lend ? lend + 1 : end;
        }
        text[ti] = '\0';
        ctx_add(ctx, start, end_t, text);
    }
    return ctx;
}

/* -----------------------------------------------------------------------
 * WebVTT parser (simplified — ignores positioning cues)
 * --------------------------------------------------------------------- */
static subs_ctx_t *parse_webvtt(const char *buf, size_t len)
{
    /* Delegate to SRT parser; timestamp format is compatible after
     * stripping the WEBVTT header line */
    const char *p = buf;
    if (len > 6 && memcmp(p, "WEBVTT", 6) == 0)
    {
        const char *nl = memchr(p, '\n', len);
        if (nl) { len -= (size_t)(nl - p + 1); p = nl + 1; }
    }
    subs_ctx_t *ctx = parse_srt(p, len);
    if (ctx) ctx->fmt = SUBS_FMT_WEBVTT;
    return ctx;
}

/* -----------------------------------------------------------------------
 * ASS/SSA parser (extracts Dialogue lines only)
 * --------------------------------------------------------------------- */
static subs_ctx_t *parse_ass(const char *buf, size_t len)
{
    subs_ctx_t *ctx = ctx_alloc(SUBS_FMT_ASS);
    if (!ctx) return NULL;

    const char *p   = buf;
    const char *end = buf + len;

    while (p < end)
    {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t llen = nl ? (size_t)(nl - p) : (size_t)(end - p);

        /* Dialogue: Layer, Start, End, Style, Name, MarginL, MarginR,
         * MarginV, Effect, Text  (10 comma-separated fields) */
        if (llen > 10 && memcmp(p, "Dialogue:", 9) == 0)
        {
            char line[1024];
            size_t copy = llen < sizeof(line) - 1 ? llen : sizeof(line) - 1;
            if (copy > 0 && p[copy-1] == '\r') copy--;
            memcpy(line, p, copy);
            line[copy] = '\0';

            /* Skip "Dialogue: " */
            char *f = line + 9;
            while (*f == ' ') f++;

            /* Skip 9 comma-separated fields to reach the text */
            int skip = 9;
            while (skip-- > 0)
            {
                f = strchr(f, ',');
                if (!f) break;
                f++;
            }

            /* Fields 1 and 2 (after layer) are start and end */
            char *fs = line + 9;
            while (*fs == ' ') fs++;
            /* field 0 = Layer */
            fs = strchr(fs, ','); if (!fs) goto next_line; fs++;
            /* field 1 = Start */
            char start_s[16]; char *fe = strchr(fs, ',');
            if (!fe) goto next_line;
            size_t slen = (size_t)(fe - fs);
            if (slen >= sizeof(start_s)) slen = sizeof(start_s) - 1;
            memcpy(start_s, fs, slen); start_s[slen] = '\0'; fs = fe + 1;
            /* field 2 = End */
            char end_s[16]; fe = strchr(fs, ',');
            if (!fe) goto next_line;
            slen = (size_t)(fe - fs);
            if (slen >= sizeof(end_s)) slen = sizeof(end_s) - 1;
            memcpy(end_s, fs, slen); end_s[slen] = '\0';

            uint32_t start_ms = 0, end_ms = 0;
            /* ASS timestamps: H:MM:SS.cs (centiseconds) */
            unsigned h, m, s, cs;
            if (sscanf(start_s, "%u:%u:%u.%u", &h, &m, &s, &cs) == 4)
                start_ms = ((h*3600u + m*60u + s)*1000u) + cs*10u;
            if (sscanf(end_s,   "%u:%u:%u.%u", &h, &m, &s, &cs) == 4)
                end_ms   = ((h*3600u + m*60u + s)*1000u) + cs*10u;

            if (f) ctx_add(ctx, start_ms, end_ms, f);
        }
next_line:
        p = nl ? nl + 1 : end;
    }
    return ctx;
}

/* -----------------------------------------------------------------------
 * Sidecar search
 * --------------------------------------------------------------------- */
bool subs_find_sidecar(const char *video_path,
                       char *out_path, size_t out_sz,
                       subs_format_t *out_fmt)
{
    /* Strip extension from video_path */
    char base[512];
    strncpy(base, video_path, sizeof(base) - 5);
    base[sizeof(base) - 5] = '\0';
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    static const struct { const char *ext; subs_format_t fmt; } exts[] = {
        { ".srt", SUBS_FMT_SRT    },
        { ".vtt", SUBS_FMT_WEBVTT },
        { ".ass", SUBS_FMT_ASS    },
        { ".ssa", SUBS_FMT_ASS    },
    };

    for (size_t i = 0; i < sizeof(exts)/sizeof(exts[0]); i++)
    {
        snprintf(out_path, out_sz, "%s%s", base, exts[i].ext);
#ifdef IPOD_VIDEO
        int fd = open(out_path, O_RDONLY);
        if (fd >= 0) { close(fd); *out_fmt = exts[i].fmt; return true; }
#else
        FILE *f = fopen(out_path, "rb");
        if (f) { fclose(f); *out_fmt = exts[i].fmt; return true; }
#endif
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Loading
 * --------------------------------------------------------------------- */
subs_ctx_t *subs_load(const char *path, subs_format_t fmt)
{
    char *buf = NULL;
    size_t len = 0;

#ifdef IPOD_VIDEO
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz <= 0 || sz > 2*1024*1024) { close(fd); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { close(fd); return NULL; }
    ssize_t got = read(fd, buf, (size_t)sz);
    close(fd);
    if (got <= 0) { free(buf); return NULL; }
    len = (size_t)got;
#else
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    if (!len || len > 2*1024*1024) { fclose(f); return NULL; }
    buf = (char *)malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    len = fread(buf, 1, len, f);
    fclose(f);
#endif
    buf[len] = '\0';

    /* Detect UTF-16 BOM and refuse (would need conversion) */
    if (len >= 2 && ((buf[0] == '\xFF' && buf[1] == '\xFE') ||
                     (buf[0] == '\xFE' && buf[1] == '\xFF')))
    {
        free(buf);
        return NULL;  /* caller may retry with iconv */
    }

    subs_ctx_t *ctx = NULL;
    switch (fmt)
    {
    case SUBS_FMT_SRT:    ctx = parse_srt(buf, len);    break;
    case SUBS_FMT_WEBVTT: ctx = parse_webvtt(buf, len); break;
    case SUBS_FMT_ASS:    ctx = parse_ass(buf, len);    break;
    default: break;
    }
    free(buf);
    return ctx;
}

subs_ctx_t *subs_load_tx3g(void)
{
    return ctx_alloc(SUBS_FMT_TX3G);
}

void subs_add_tx3g_cue(subs_ctx_t *ctx,
                       const uint8_t *data, size_t len,
                       uint32_t start_ms, uint32_t dur_ms)
{
    if (!ctx || !data || len < 2) return;
    /* tx3g sample: 2-byte big-endian length, then UTF-8 text */
    uint16_t text_len = ((uint16_t)data[0] << 8) | data[1];
    if ((size_t)text_len + 2u > len) text_len = (uint16_t)(len - 2);
    char text[SUBS_MAX_TEXT];
    size_t copy = text_len < sizeof(text) - 1 ? text_len : sizeof(text) - 1;
    memcpy(text, data + 2, copy);
    text[copy] = '\0';
    ctx_add(ctx, start_ms, start_ms + dur_ms, text);
}

void subs_free(subs_ctx_t *ctx)
{
    if (!ctx) return;
    free(ctx->cues);
    free(ctx);
}

/* -----------------------------------------------------------------------
 * Lookup (binary search)
 * --------------------------------------------------------------------- */
const subs_cue_t *subs_get_at(const subs_ctx_t *ctx, uint32_t time_ms)
{
    if (!ctx || !ctx->ncues) return NULL;

    int lo = 0, hi = ctx->ncues - 1;
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        const subs_cue_t *c = &ctx->cues[mid];
        if (time_ms < c->start_ms)      hi = mid - 1;
        else if (time_ms >= c->end_ms)  lo = mid + 1;
        else                             return c;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Rendering
 * --------------------------------------------------------------------- */
int subs_render(uint32_t image_handle,
                int image_w, int image_h,
                const char *text)
{
#ifndef IPOD_VIDEO
    (void)image_handle; (void)image_w; (void)image_h; (void)text;
    return 0;
#else
    /* Clear the bottom quarter of the image */
    int sub_y = image_h * 3 / 4;
    int sub_h = image_h - sub_y;

    /* Clear area: write a black transparent rectangle */
    uint32_t clear_args[] = {
        (uint32_t)image_handle,
        0, (uint32_t)sub_y,
        (uint32_t)image_w, (uint32_t)sub_h,
        0x00000000u,  /* ARGB: transparent black */
    };
    bcm_gfx_send(0x09u /* fill rect */, clear_args, 6, NULL);

    if (!text || !*text) return 0;

    /* Render each line of text using Helvetica Bold (added in v90) */
    /* For now, write white text at the bottom centre */
    uint32_t text_args[] = {
        (uint32_t)image_handle,
        (uint32_t)(image_w / 2),
        (uint32_t)(sub_y + sub_h / 2),
        0xFFFFFFFFu,   /* white */
        0x00000080u,   /* semi-transparent black background */
    };
    /* Send text as extra payload */
    uint32_t buf[64];
    memcpy(buf, text_args, sizeof(text_args));
    size_t tlen = strlen(text);
    if (tlen > sizeof(buf) - sizeof(text_args) - 1)
        tlen = sizeof(buf) - sizeof(text_args) - 1;
    memcpy((char *)buf + sizeof(text_args), text, tlen + 1);
    bcm_gfx_send(0x0Au /* draw text */, buf,
                 (int)((sizeof(text_args) + tlen + 1 + 3) / 4), NULL);

    return 0;
#endif
}
