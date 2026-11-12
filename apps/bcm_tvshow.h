/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * TV-series episode ordering for auto-play.
 * Parses SxxEyy / 1x02 patterns in filenames to determine the
 * next episode in a TV-series directory.
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
#ifndef BCM_TVSHOW_H
#define BCM_TVSHOW_H

#include <stdbool.h>

/**
 * tvshow_next_episode - given the path of the just-played episode, find
 * the next one in the same directory using episode ordering.
 *
 * Parses SxxEyy / NxMM patterns.  Falls back to alphabetical order if
 * no episode pattern is found.
 *
 * @current_path : full path of the episode just played
 * @next_path    : buffer to receive the next episode's full path
 * @next_sz      : size of @next_path
 *
 * Returns true if a next episode was found.
 */
bool tvshow_next_episode(const char *current_path,
                         char *next_path, size_t next_sz);

/**
 * tvshow_parse_ep - extract season and episode numbers from @filename.
 * Returns true if a pattern was found.
 */
bool tvshow_parse_ep(const char *filename, int *season, int *episode);

#endif /* BCM_TVSHOW_H */
