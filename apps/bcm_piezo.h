/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * Start-up / shutdown / charging chimes via the iPod's click speaker.
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
#ifndef BCM_PIEZO_H
#define BCM_PIEZO_H

#include <stdbool.h>

/* Chime event type */
typedef enum {
    PIEZO_CHIME_STARTUP  = 0,
    PIEZO_CHIME_SHUTDOWN = 1,
    PIEZO_CHIME_CHARGE   = 2,
    PIEZO_CHIME_CUSTOM   = 3,  /* loaded from a WAV file                  */
} piezo_chime_t;

/**
 * bcm_piezo_init - initialise the chime subsystem.
 * Must be called once from main before any chime is requested.
 */
void bcm_piezo_init(void);

/**
 * bcm_piezo_play - play a built-in chime asynchronously.
 * Returns immediately; the sample plays in the background via the
 * click-speaker PCM sink.
 */
void bcm_piezo_play(piezo_chime_t chime);

/**
 * bcm_piezo_play_wav - play a WAV file through the click speaker.
 * @path : absolute path to a 16-bit mono PCM WAV file (any sample rate).
 */
void bcm_piezo_play_wav(const char *path);

/**
 * bcm_piezo_stop - stop any currently playing chime.
 */
void bcm_piezo_stop(void);

#endif /* BCM_PIEZO_H */
