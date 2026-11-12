/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * TV-series episode ordering.
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

#include "bcm_tvshow.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#ifdef IPOD_VIDEO
#include "dir.h"
#include "file.h"
#endif

/* -----------------------------------------------------------------------
 * Episode number parsing
 * Accepts: S01E02, s1e2, 1x02, 01x02
 * --------------------------------------------------------------------- */
bool tvshow_parse_ep(const char *filename, int *season, int *episode)
{
    if (!filename) return false;

    /* Case-insensitive scan */
    for (const char *p = filename; *p; p++)
    {
        /* SxxEyy pattern */
        if (tolower((unsigned char)*p) == 's' && isdigit((unsigned char)p[1]))
        {
            int s = 0, e = 0;
            const char *q = p + 1;
            while (isdigit((unsigned char)*q)) s = s*10 + (*q++ - '0');
            if (tolower((unsigned char)*q) == 'e' && isdigit((unsigned char)q[1]))
            {
                q++;
                while (isdigit((unsigned char)*q)) e = e*10 + (*q++ - '0');
                if (s > 0 && e > 0)
                {
                    *season = s; *episode = e;
                    return true;
                }
            }
        }
        /* NxMM pattern: digit(s) 'x' digit(s) */
        if (isdigit((unsigned char)*p) && tolower((unsigned char)p[1]) == 'x' &&
            isdigit((unsigned char)p[2]))
        {
            int s = 0, e = 0;
            const char *q = p;
            while (isdigit((unsigned char)*q)) s = s*10 + (*q++ - '0');
            q++; /* skip 'x' */
            while (isdigit((unsigned char)*q)) e = e*10 + (*q++ - '0');
            if (s > 0 && e > 0)
            {
                *season = s; *episode = e;
                return true;
            }
        }
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Episode comparison key: season*10000 + episode
 * --------------------------------------------------------------------- */
static int ep_key(const char *filename)
{
    int s = 0, e = 0;
    if (tvshow_parse_ep(filename, &s, &e))
        return s * 10000 + e;
    return -1;
}

/* -----------------------------------------------------------------------
 * Video file extensions
 * --------------------------------------------------------------------- */
static bool is_video_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    const char *exts[] = { ".mp4", ".m4v", ".mov", ".avi", ".mkv", NULL };
    for (int i = 0; exts[i]; i++)
    {
        if (strcasecmp(dot, exts[i]) == 0)
            return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * tvshow_next_episode
 * --------------------------------------------------------------------- */
bool tvshow_next_episode(const char *current_path,
                         char *next_path, size_t next_sz)
{
    if (!current_path || !next_path) return false;

    /* Split directory and filename */
    char dir[512];
    strncpy(dir, current_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (!slash) return false;
    *slash = '\0';
    const char *cur_name = slash + 1;

    int cur_key = ep_key(cur_name);

#ifdef IPOD_VIDEO
    DIR *dp = opendir(dir);
    if (!dp) return false;

    char   best_name[256] = "";
    int    best_key = INT_MAX;
    bool   found = false;
    struct dirent *de;

    while ((de = readdir(dp)) != NULL)
    {
        if (!is_video_ext(de->d_name)) continue;
        if (strcmp(de->d_name, cur_name) == 0) continue;

        int k = ep_key(de->d_name);
        if (k < 0) continue;  /* no episode pattern */

        /* Next episode: smallest key that is greater than cur_key */
        if (cur_key >= 0)
        {
            if (k > cur_key && k < best_key)
            {
                best_key = k;
                strncpy(best_name, de->d_name, sizeof(best_name) - 1);
                found = true;
            }
        }
        else
        {
            /* No pattern in current name: alphabetical next */
            if (strcmp(de->d_name, cur_name) > 0 &&
                (best_name[0] == '\0' || strcmp(de->d_name, best_name) < 0))
            {
                strncpy(best_name, de->d_name, sizeof(best_name) - 1);
                found = true;
            }
        }
    }
    closedir(dp);

    if (found)
        snprintf(next_path, next_sz, "%s/%s", dir, best_name);
    return found;
#else
    /* PC stub */
    (void)next_sz;
    (void)cur_key;
    return false;
#endif
}
