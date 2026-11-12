/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * Chime manager — loads built-in formant samples and WAV files,
 * feeds them to the click-speaker PCM sink in piezo.c.
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

#include "bcm_piezo.h"
#include "bcm_piezo_voice.h"
#include <string.h>
#include <stdlib.h>

#ifdef IPOD_VIDEO
#include "file.h"
#include "kernel.h"
#include "system.h"
/* bcm_piezo_set_sample is declared in firmware/target/arm/ipod/piezo.h */
#include "piezo.h"
#endif

/* -----------------------------------------------------------------------
 * Built-in voice sample cache
 * --------------------------------------------------------------------- */
#define MAX_CACHE_SAMPLES 4

typedef struct {
    piezo_chime_t   chime;
    const int16_t  *data;
    uint32_t        len;    /* number of int16_t samples                  */
    uint32_t        rate;   /* sample rate in Hz                          */
} chime_cache_t;

static chime_cache_t s_cache[MAX_CACHE_SAMPLES];
static int           s_cache_count;
static bool          s_inited;

/* -----------------------------------------------------------------------
 * WAV header parser (PCM only)
 * --------------------------------------------------------------------- */
typedef struct {
    uint16_t audio_format;    /* 1 = PCM                                  */
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_fmt_t;

static bool parse_wav_header(const uint8_t *buf, size_t buf_len,
                              wav_fmt_t *fmt, size_t *data_offset,
                              size_t *data_bytes)
{
    if (buf_len < 44) return false;
    if (memcmp(buf, "RIFF", 4) != 0) return false;
    if (memcmp(buf + 8, "WAVE", 4) != 0) return false;

    size_t pos = 12;
    while (pos + 8 <= buf_len)
    {
        uint32_t chunk_size;
        memcpy(&chunk_size, buf + pos + 4, 4);

        if (memcmp(buf + pos, "fmt ", 4) == 0 && chunk_size >= 16)
        {
            memcpy(&fmt->audio_format,   buf + pos + 8,  2);
            memcpy(&fmt->channels,       buf + pos + 10, 2);
            memcpy(&fmt->sample_rate,    buf + pos + 12, 4);
            memcpy(&fmt->byte_rate,      buf + pos + 16, 4);
            memcpy(&fmt->block_align,    buf + pos + 20, 2);
            memcpy(&fmt->bits_per_sample,buf + pos + 22, 2);
        }
        else if (memcmp(buf + pos, "data", 4) == 0)
        {
            *data_offset = pos + 8;
            *data_bytes  = chunk_size;
            return (fmt->audio_format == 1);
        }
        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++;  /* word-align */
    }
    return false;
}

/* -----------------------------------------------------------------------
 * bcm_piezo_init
 * --------------------------------------------------------------------- */
void bcm_piezo_init(void)
{
    if (s_inited) return;
    s_inited = true;
    s_cache_count = 0;

    /* Register built-in formant samples */
    static const struct {
        piezo_chime_t chime;
        const int16_t *data;
        uint32_t       len;
        uint32_t       rate;
    } builtin[] = {
        { PIEZO_CHIME_STARTUP,  piezo_voice_startup,
          PIEZO_VOICE_STARTUP_SAMPLES, PIEZO_VOICE_RATE },
        { PIEZO_CHIME_SHUTDOWN, piezo_voice_shutdown,
          PIEZO_VOICE_SHUTDOWN_SAMPLES, PIEZO_VOICE_RATE },
        { PIEZO_CHIME_CHARGE,   piezo_voice_charge,
          PIEZO_VOICE_CHARGE_SAMPLES,  PIEZO_VOICE_RATE },
    };

    for (size_t i = 0; i < sizeof(builtin)/sizeof(builtin[0]) &&
                        s_cache_count < MAX_CACHE_SAMPLES; i++)
    {
        s_cache[s_cache_count].chime = builtin[i].chime;
        s_cache[s_cache_count].data  = builtin[i].data;
        s_cache[s_cache_count].len   = builtin[i].len;
        s_cache[s_cache_count].rate  = builtin[i].rate;
        s_cache_count++;
    }
}

/* -----------------------------------------------------------------------
 * bcm_piezo_play
 * --------------------------------------------------------------------- */
void bcm_piezo_play(piezo_chime_t chime)
{
    if (!s_inited) bcm_piezo_init();

    for (int i = 0; i < s_cache_count; i++)
    {
        if (s_cache[i].chime == chime)
        {
#ifdef IPOD_VIDEO
            bcm_piezo_set_sample(s_cache[i].data,
                                 s_cache[i].len,
                                 s_cache[i].rate);
#endif
            return;
        }
    }
}

/* -----------------------------------------------------------------------
 * bcm_piezo_play_wav
 * --------------------------------------------------------------------- */
void bcm_piezo_play_wav(const char *path)
{
    if (!path) return;

#ifdef IPOD_VIDEO
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz <= 0 || sz > 512 * 1024) { close(fd); return; }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { close(fd); return; }

    ssize_t got = read(fd, buf, (size_t)sz);
    close(fd);

    if (got <= 44)
    {
        free(buf);
        return;
    }

    wav_fmt_t fmt = { 0 };
    size_t data_off = 0, data_bytes = 0;
    if (!parse_wav_header(buf, (size_t)got, &fmt, &data_off, &data_bytes) ||
        fmt.bits_per_sample != 16 || fmt.channels != 1)
    {
        free(buf);
        return;
    }

    /* Copy samples to a persistent buffer (freed when play finishes) */
    size_t nsamples = data_bytes / 2;
    int16_t *samples = (int16_t *)malloc(nsamples * 2);
    if (!samples) { free(buf); return; }

    memcpy(samples, buf + data_off, nsamples * 2);
    free(buf);

    /* Register as a custom cache entry (overwrite slot 3) */
    int slot = MAX_CACHE_SAMPLES - 1;
    if (s_cache[slot].data && s_cache[slot].chime == PIEZO_CHIME_CUSTOM)
        free((void *)s_cache[slot].data);  /* cast away const for malloc'd buf */

    s_cache[slot].chime = PIEZO_CHIME_CUSTOM;
    s_cache[slot].data  = samples;
    s_cache[slot].len   = (uint32_t)nsamples;
    s_cache[slot].rate  = fmt.sample_rate;

    bcm_piezo_set_sample(samples, (uint32_t)nsamples, fmt.sample_rate);
#else
    (void)path;
#endif
}

/* -----------------------------------------------------------------------
 * bcm_piezo_stop
 * --------------------------------------------------------------------- */
void bcm_piezo_stop(void)
{
#ifdef IPOD_VIDEO
    bcm_piezo_set_sample(NULL, 0, 0);
#endif
}
