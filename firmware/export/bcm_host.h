/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * BCM VideoCore host API — bcm-rockbox v94
 *
 * Public interface to the Broadcom BCM2722 VideoCore II co-processor
 * fitted in iPod Video (5G / 5.5G).  All functions are only compiled
 * when HAVE_BCM_VIDEOCORE is defined (ipodvideo.h sets it).
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

#ifndef BCM_HOST_H
#define BCM_HOST_H

#include "config.h"

#ifdef HAVE_BCM_VIDEOCORE

#include <stdbool.h>
#include "lcd.h"   /* fb_data */

/* ---- Low-level gencmd / mailbox ---- */

/* Send an ASCII command string to the BCM firmware and read the reply.
 * Returns 0 on success, negative on error. */
int  bcm_gencmd(const char *cmd, char *reply, int reply_len);

/* Returns true if the BCM is currently powered and active. */
bool bcm_is_active(void);

/* ---- VMCS image management ---- */

/* Replace the VMCS firmware image the BCM runs after power-up.
 * Pass image=NULL, length=0 to restore the factory image from flash. */
void bcm_use_vmcs_image(const fb_data *image, unsigned length);

/* ---- LCD freeze for video overlay ---- */

/* Suspend BCM-issued LCD refreshes (hold the tick mutex).
 * The caller is responsible for painting its own frames while frozen. */
void bcm_lcd_freeze(void);

/* Resume BCM-issued LCD refreshes and kick an immediate update. */
void bcm_lcd_unfreeze(void);

/* ---- H.264 video player ---- */

/* Start playing filename from start_ms milliseconds.
 * Returns 0 on success, negative on error. */
int  bcm_video_quiz_start(const char *filename, int start_ms);

/* Stop the currently playing video (no-op if none). */
void bcm_video_quiz_stop(void);

/* Return the current playback position in milliseconds, or -1. */
int  bcm_video_quiz_get_pos_ms(void);

/* Register the actual player implementation (called by bcm_video_player.c).
 * The firmware layer holds only function pointers; the app layer supplies
 * the real functions via this call so the firmware doesn't pull in app code. */
void bcm_video_quiz_register(
    int  (*start_fn)(const char*, int),
    void (*stop_fn)(void),
    int  (*get_pos_fn)(void));

/* ---- Init (called from lcd_init_device) ---- */
void bcm_api_init(void);

#endif /* HAVE_BCM_VIDEOCORE */
#endif /* BCM_HOST_H */
