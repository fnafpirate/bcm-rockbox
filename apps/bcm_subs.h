/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * Subtitle parser and renderer for bcm-rockbox.
 * Supports SRT, WebVTT, ASS/SSA, and MP4 tx3g in-band subtitles.
 * Renders into a VideoCore image overlay via bcm_gfx_send().
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
#ifndef BCM_SUBS_H
#define BCM_SUBS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum subtitle text per cue (UTF-8) */
#define SUBS_MAX_TEXT   512

/* Maximum number of loaded cues */
#define SUBS_MAX_CUES   4096

/* Subtitle format */
typedef enum {
    SUBS_FMT_NONE   = 0,
    SUBS_FMT_SRT    = 1,
    SUBS_FMT_WEBVTT = 2,
    SUBS_FMT_ASS    = 3,
    SUBS_FMT_TX3G   = 4,   /* in-band MP4 tx3g                            */
} subs_format_t;

/* One subtitle cue */
typedef struct {
    uint32_t start_ms;          /* presentation start in milliseconds     */
    uint32_t end_ms;            /* presentation end                       */
    char     text[SUBS_MAX_TEXT]; /* plain text (HTML/ASS tags stripped)  */
} subs_cue_t;

/* Subtitle context (opaque to caller) */
typedef struct subs_ctx subs_ctx_t;

/* -----------------------------------------------------------------------
 * Sidecar file search
 * --------------------------------------------------------------------- */

/**
 * subs_find_sidecar - look for a subtitle sidecar next to a video file.
 *
 * Tries <video_base>.srt, .vtt, .ass, .ssa in that order.
 * @video_path : full path to the video file
 * @out_path   : buffer to receive the sidecar path (size @out_sz)
 * @out_fmt    : set to the detected format
 *
 * Returns true if a sidecar was found.
 */
bool subs_find_sidecar(const char *video_path,
                       char *out_path, size_t out_sz,
                       subs_format_t *out_fmt);

/* -----------------------------------------------------------------------
 * Loading
 * --------------------------------------------------------------------- */

/**
 * subs_load - allocate a context and parse a sidecar subtitle file.
 * Returns NULL on failure.
 */
subs_ctx_t *subs_load(const char *path, subs_format_t fmt);

/**
 * subs_load_tx3g - create a context for in-band tx3g subtitles.
 * Individual cues are added via subs_add_tx3g_cue().
 */
subs_ctx_t *subs_load_tx3g(void);

/**
 * subs_add_tx3g_cue - append one tx3g sample.
 * @ctx        : context from subs_load_tx3g()
 * @data       : raw tx3g sample data
 * @len        : byte length of @data
 * @start_ms   : presentation timestamp
 * @dur_ms     : duration
 */
void subs_add_tx3g_cue(subs_ctx_t *ctx,
                       const uint8_t *data, size_t len,
                       uint32_t start_ms, uint32_t dur_ms);

/**
 * subs_free - free all resources associated with a context.
 */
void subs_free(subs_ctx_t *ctx);

/* -----------------------------------------------------------------------
 * Lookup
 * --------------------------------------------------------------------- */

/**
 * subs_get_at - return the cue active at @time_ms, or NULL if none.
 * Does a binary search; O(log N).
 */
const subs_cue_t *subs_get_at(const subs_ctx_t *ctx, uint32_t time_ms);

/* -----------------------------------------------------------------------
 * Rendering into a VideoCore image overlay
 * --------------------------------------------------------------------- */

/**
 * subs_render - draw @text into the subtitle image overlay.
 *
 * Uses the VC image handle @image_handle (created with bcm_gfx op8).
 * Clears the previous subtitle area first.
 * @text may be NULL to clear only.
 *
 * Returns 0 on success.
 */
int subs_render(uint32_t image_handle,
                int image_w, int image_h,
                const char *text);

#ifdef __cplusplus
}
#endif

#endif /* BCM_SUBS_H */
