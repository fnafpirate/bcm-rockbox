/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * Dock state interface between the iAP driver and the video player.
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
#ifndef BCM_DOCK_H
#define BCM_DOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Dock capability flags reported by the accessory via iAP lingo 0 */
#define DOCK_CAP_LINEOUT    (1u << 0)
#define DOCK_CAP_VIDEOOUT   (1u << 1)
#define DOCK_CAP_REMOTE     (1u << 2)

typedef struct {
    bool     connected;         /* true if an iAP accessory is attached    */
    uint32_t caps;              /* DOCK_CAP_* flags                        */
    uint8_t  accessory_id[8];   /* raw accessory ID bytes                  */
    char     accessory_name[64];
} dock_state_t;

/* Global dock state — written by iap-core.c, read by the video player. */
extern dock_state_t g_dock_state;

/** Called by iAP when the dock state changes. */
void bcm_dock_notify(const dock_state_t *new_state);

#endif /* BCM_DOCK_H */
