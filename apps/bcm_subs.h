/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * bcm overlay v73: soft subtitles for the iPod Video (5G / 5.5G) video player
 *
 * Text subtitles drawn by the iPod while a video plays, never burned in:
 *   - sidecar files next to the video: Name.srt, Name.vtt, Name.ass, Name.ssa,
 *     also Name.<anything>.<ext> ("Name.en.srt", "Name.English.forced.srt")
 *   - MP4 timed-text tracks (tx3g) inside the video (loaded by the player,
 *     which owns the MP4 parser, through subs_add())
 * Styling kept: italic, colours, "top of the screen" cues ({\an8}, ASS \an7-9,
 * WebVTT line:0%).  Everything else (fonts, sizes, karaoke, positions,
 * drawings) is dropped -- the text is drawn in a Rockbox font with an outline
 * or a box, over the picture.
 *
 * Plan of record this follows and extends: ProkBox5g docs/design/spec/videos.md
 * section 4 (GPL v2+, prokbox5g), which sets the source order, the sidecar
 * naming and the parser edge cases (byte-order mark, CRLF, overlapping cues,
 * <i> tags, hearing-impaired cues).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#ifndef BCM_SUBS_H
#define BCM_SUBS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum { SUBS_NONE = 0, SUBS_SRT, SUBS_VTT, SUBS_ASS, SUBS_TX3G };

/* cue flags */
#define SUBS_TOP   0x0001          /* drawn at the top of the picture */

/* in-text style codes of the stored (normalised UTF-8) text */
#define SUBS_C_ITALIC_ON   0x01
#define SUBS_C_ITALIC_OFF  0x02
#define SUBS_C_COLOR       0x03    /* followed by 3 bytes R G B (never 0: 0 is stored as 1) */
#define SUBS_C_COLOR_OFF   0x04

#define SUBS_MAX_TRACKS 10
#define SUBS_PATH_MAX   260

struct subs_track {
    uint8_t  kind;                 /* SUBS_SRT ... SUBS_TX3G */
    uint8_t  index;                /* tx3g: which text track in the MP4 (0 = first) */
    char     label[40];            /* shown when switching: "English", "en forced", "SRT" ... */
    char     path[SUBS_PATH_MAX];  /* sidecar file (empty for tx3g) */
};

struct subs_cue {                  /* one stored cue (followed by its text) */
    uint32_t start, end;           /* ms */
    uint16_t len, flags;           /* text bytes, SUBS_TOP */
};

struct subs_store {
    uint8_t  *buf;
    uint32_t size, used;           /* records grow from buf */
    uint32_t *idx;                 /* after subs_finish: record offsets sorted by start */
    uint32_t n;
    uint32_t maxdur;               /* longest cue (ms), bounds the active-cue search */
};

/* ---- finding subtitles */
/* sidecar files for 'video' (absolute path); best first.  Returns the count. */
int subs_find_sidecars(const char *video, struct subs_track *t, int max);

/* ---- the store (one buffer, caller-owned) */
void subs_store_init(struct subs_store *s, void *buf, uint32_t size);
/* add a cue; the text may contain SRT/HTML style tags ("<i>", "<font color=...>",
   "{\an8}") and CRLF: it is normalised.  false = buffer full. */
bool subs_add(struct subs_store *s, uint32_t start, uint32_t end, const char *text, uint32_t len, uint16_t flags);
void subs_finish(struct subs_store *s);
const struct subs_cue *subs_cue_at(const struct subs_store *s, uint32_t i);   /* sorted order */
static inline const char *subs_text(const struct subs_cue *c) { return (const char *)(c + 1); }
/* the cues showing at 'ms' (up to max, in start order); returns the count */
int subs_active(const struct subs_store *s, uint32_t ms, const struct subs_cue **out, int max);

/* ---- loading a sidecar file into the store (the buffer must be big enough:
   subs_buffer_size(file size)); returns the cue count, < 0 on error */
uint32_t subs_buffer_size(uint32_t file_size);
int subs_load_file(struct subs_store *s, const char *path, int kind);
/* parse text already in memory (used by subs_load_file and the host tests).
   'raw' may be anywhere; it is converted to UTF-8 first if it is UTF-16 or
   not valid UTF-8 (then read as Windows-1252). */
int subs_parse(struct subs_store *s, const uint8_t *raw, uint32_t len, int kind);

/* ---- drawing */
struct subs_font {
    int height, ascent, depth;     /* depth 0: 1-bit Rockbox columns; 1: 4-bit alpha rows */
    int (*width)(void *ctx, uint32_t ucs);
    const uint8_t *(*bits)(void *ctx, uint32_t ucs);
    void *ctx;
};
struct subs_style {
    int scale;                     /* 1 = font pixels, 2 = doubled (TV) */
    int outline;                   /* outline radius in output pixels (0 = none) */
    bool box;                      /* a dark box behind each line instead of the outline */
    uint32_t color;                /* default text colour 0xRRGGBB */
    int margin;                    /* left/right margin */
};
/* Draw the cues into an ARGB canvas (cleared first).  The block of lines ends
   at row 'bottom' (bottom cues) or starts at row 'top' (all cues SUBS_TOP).
   'msg' (optional) is drawn instead of the cues (a short notice).
   Returns the rows used [*y0, *y1) or 0 when nothing was drawn.
   'work' needs w*h bytes (alpha mask). */
int subs_render(uint32_t *canvas, uint8_t *work, int w, int h, const struct subs_font *f,
                const struct subs_style *st, const struct subs_cue **cues, int n, const char *msg,
                int top, int bottom, int *y0, int *y1);

#endif
