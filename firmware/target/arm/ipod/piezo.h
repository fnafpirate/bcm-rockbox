/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006-2007 Robert Keevil
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

void piezo_init(void);
void piezo_play(unsigned short inv_freq, unsigned char form);
void piezo_play_for_tick(unsigned short inv_freq,
                    unsigned char form, unsigned int dur);
void piezo_play_for_usec(unsigned short inv_freq,
                    unsigned char form, unsigned int dur);
void piezo_stop(void);
void piezo_clear(void);
bool piezo_busy(void);
unsigned int piezo_hz(unsigned int hz);
void piezo_button_beep(bool beep, bool force);
/* bcm overlay v77 (piezo.c) */
bool piezo_speaker_active(void);
int piezo_speaker_home_sink(void);              /* enum pcm_sink_ids: PCM_SINK_PIEZO or PCM_SINK_BUILTIN */
void piezo_play_tune(const unsigned short *notes);   /* {hz, ms} ..., {0, 0}; hz 0 = rest */
void piezo_play_samples(const unsigned char *u8, unsigned n, unsigned rate);   /* 8-bit unsigned at `rate` Hz */
void piezo_settings(int method, int carrier, int boost, int speaker, int quality, int loudness);
extern volatile bool piezo_docked;
