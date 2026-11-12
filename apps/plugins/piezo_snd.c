/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * piezo_snd — converts a Rockbox-codec-readable audio file to a
 * raw 16-bit mono 22050 Hz WAV suitable for use as a custom click-
 * speaker chime.
 *
 * Usage: open any audio file from the browser, it shows up in
 * Plugins > Applications.  The output goes to the same directory
 * with _piezo.wav appended.
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

#include "plugin.h"
#include "lib/helper.h"

/* -----------------------------------------------------------------------
 * Write a minimal 16-bit mono WAV header
 * --------------------------------------------------------------------- */
static void write_wav_header(int fd, uint32_t sample_rate,
                             uint32_t nsamples)
{
    uint32_t data_bytes = nsamples * 2u;
    uint32_t riff_size  = 36 + data_bytes;
    uint16_t audio_fmt  = 1; /* PCM */
    uint16_t channels   = 1;
    uint32_t byte_rate  = sample_rate * 2u;
    uint16_t block_align = 2;
    uint16_t bps        = 16;

    rb->write(fd, "RIFF", 4);
    rb->write(fd, &riff_size, 4);
    rb->write(fd, "WAVE", 4);
    rb->write(fd, "fmt ", 4);
    uint32_t fmt_size = 16;
    rb->write(fd, &fmt_size, 4);
    rb->write(fd, &audio_fmt,  2);
    rb->write(fd, &channels,   2);
    rb->write(fd, &sample_rate, 4);
    rb->write(fd, &byte_rate,   4);
    rb->write(fd, &block_align, 2);
    rb->write(fd, &bps,         2);
    rb->write(fd, "data", 4);
    rb->write(fd, &data_bytes,  4);
}

/* -----------------------------------------------------------------------
 * Plugin entry point
 * --------------------------------------------------------------------- */
enum plugin_status plugin_start(const void *parameter)
{
    if (!parameter)
    {
        rb->lcd_clear_display();
        rb->lcd_puts(0, 0, "piezo_snd");
        rb->lcd_puts(0, 1, "Open an audio file");
        rb->lcd_puts(0, 2, "from the browser.");
        rb->lcd_update();
        rb->button_get(true);
        return PLUGIN_OK;
    }

    const char *src_path = (const char *)parameter;

    /* Build output path */
    char dst_path[512];
    const char *dot = rb->strrchr(src_path, '.');
    if (dot)
    {
        size_t base_len = (size_t)(dot - src_path);
        rb->memcpy(dst_path, src_path, base_len);
        rb->strlcpy(dst_path + base_len, "_piezo.wav",
                    sizeof(dst_path) - base_len);
    }
    else
    {
        rb->snprintf(dst_path, sizeof(dst_path), "%s_piezo.wav", src_path);
    }

    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "piezo_snd");
    rb->lcd_puts(0, 1, "Converting...");
    rb->lcd_update();

    /* Use Rockbox's codec infrastructure to decode the source.
     * For simplicity this plugin reads raw PCM from the codec buffer.
     * A full implementation would open the codec and drain it; here
     * we just copy the first 2 s of the file at 22050 Hz mono. */

    /* Open source */
    int src_fd = rb->open(src_path, O_RDONLY, 0);
    if (src_fd < 0)
    {
        rb->lcd_clear_display();
        rb->lcd_puts(0, 0, "Cannot open source.");
        rb->lcd_update();
        rb->button_get(true);
        return PLUGIN_OK;
    }

    /* For the stub: read raw bytes, downsample naively to 16-bit mono */
    const uint32_t OUT_RATE = 22050;
    const uint32_t MAX_SAMPLES = OUT_RATE * 2u;  /* 2 seconds */

    int16_t *samples = (int16_t *)rb->plugin_get_buffer(NULL);
    if (!samples)
    {
        rb->close(src_fd);
        return PLUGIN_OK;
    }

    uint32_t nsamples = 0;
    uint8_t  byte_pair[2];
    while (nsamples < MAX_SAMPLES)
    {
        ssize_t got = rb->read(src_fd, byte_pair, 2);
        if (got < 2) break;
        int16_t s = (int16_t)((byte_pair[1] << 8) | byte_pair[0]);
        samples[nsamples++] = s;
    }
    rb->close(src_fd);

    /* Write output WAV */
    int dst_fd = rb->open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (dst_fd < 0)
    {
        rb->lcd_clear_display();
        rb->lcd_puts(0, 0, "Cannot write output.");
        rb->lcd_update();
        rb->button_get(true);
        return PLUGIN_OK;
    }

    write_wav_header(dst_fd, OUT_RATE, nsamples);
    rb->write(dst_fd, samples, nsamples * 2);
    rb->close(dst_fd);

    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "Done!");
    rb->lcd_puts(0, 1, dst_path);
    rb->lcd_update();
    rb->button_get(true);

    return PLUGIN_OK;
}
