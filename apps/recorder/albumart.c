/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 Nicolas Pennequin
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

#include <stdio.h>
#include "string-extra.h"
#include "system.h"
#include "albumart.h"
#include "metadata.h"
#include "buffering.h"
#include "dircache.h"
#include "misc.h"
#include "pathfuncs.h"
#include "settings.h"
#include "wps.h"

/* Define LOGF_ENABLE to enable logf output in this file */
/*#define LOGF_ENABLE*/
#include "logf.h"

#if defined(HAVE_JPEG) || defined(PLUGIN)
#define USE_JPEG_COVER
#endif

#ifdef PLUGIN
    #define strmemccpy strlcpy
    /* Note we don't use the return value so this works */
    /* FIXME if strmemccpy gets added to the rb->plugin struct */
#endif

/* Strip filename from a full path
 *
 * buf      - buffer to extract directory to.
 * buf_size - size of buffer.
 * fullpath - fullpath to extract from.
 *
 * Split the directory part of the given fullpath and store it in buf
 *   (including last '/').
 * The function return parameter is a pointer to the filename
 *   inside the given fullpath.
 */
static const char* strip_filename(char* buf, int buf_size, const char* fullpath)
{
    const char* sep;
    int   len;

    if (!buf || buf_size <= 0 || !fullpath)
        return NULL;

    /* if 'fullpath' is only a filename return immediately */
    sep = strrchr(fullpath, '/');
    if (sep == NULL)
    {
        buf[0] = 0;
        return (char*)fullpath;
    }

    len = MIN(sep - fullpath + 1, buf_size - 1);
    strmemccpy(buf, fullpath, len + 1);
    return (sep + 1);
}

#if !defined(PLUGIN) && !defined(SIMULATOR) && defined(IPOD_VIDEO)
/* bcm overlay v139: a track without cover data costs up to 18 file_exists() calls per album art search (sized and
   generic names, 3 extensions, this folder and its parent) -- 12 of them in the track's own folder.  Each one reads
   the whole folder when the directory cache is off or full (a big library): with 2000 songs in one folder that was
   ~2 MB of directory entries, read twice per track change (this track and the next one), on the audio thread.  Now a
   folder is read once and its image file names (.jpg .jpeg .bmp) are kept for 2 minutes; every look-up in it is a
   string compare.  A folder with more images than fit falls back to file_exists(). */
#include "dir.h"
#include "file.h"
#define AA_DIRS 3
#define AA_POOL 2048
static struct { char dir[MAX_PATH]; long t; bool overflow; int used; char pool[AA_POOL]; } aa_c[AA_DIRS];
static int aa_next;
static unsigned aa_lookups, aa_scans;
static struct mutex aa_mtx;
static bool aa_mtx_ok;
static bool aa_fe(const char *p) { return file_exists(p); }   /* the real one */
static bool aa_is_image(const char *name)
{
    const char *e = strrchr(name, '.');
    return e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".bmp"));
}
static int aa_dir(const char *dir)
{
    int i;
    DIR *d;
    struct dirent *de;
    for (i = 0; i < AA_DIRS; i++)
        if (aa_c[i].dir[0] && !strcmp(aa_c[i].dir, dir) && current_tick - aa_c[i].t < 120 * HZ)
            return i;
    i = aa_next; aa_next = (aa_next + 1) % AA_DIRS;
    aa_c[i].dir[0] = 0; aa_c[i].used = 0; aa_c[i].overflow = false;
    aa_scans++;
    d = opendir(dir[0] ? dir : "/");
    if (!d)
        return -1;
    while ((de = readdir(d)))
    {
        size_t n = strlen((const char *)de->d_name) + 1;
        if (!aa_is_image((const char *)de->d_name))
            continue;
        if (aa_c[i].used + (int)n > AA_POOL) { aa_c[i].overflow = true; break; }
        memcpy(aa_c[i].pool + aa_c[i].used, de->d_name, n);
        aa_c[i].used += n;
    }
    closedir(d);
    strmemccpy(aa_c[i].dir, dir, sizeof(aa_c[i].dir));
    aa_c[i].t = current_tick;
    return i;
}
static bool aa_exists(const char *path)
{
    char dir[MAX_PATH];
    const char *name = strrchr(path, '/');
    bool found = false;
    int i, k;
    aa_lookups++;
    if (!name || (size_t)(name - path) >= sizeof(dir))
        return aa_fe(path);
    if (!aa_mtx_ok)
    {
        int old = disable_irq_save();
        if (!aa_mtx_ok) { mutex_init(&aa_mtx); aa_mtx_ok = true; }
        restore_irq(old);
    }
    memcpy(dir, path, name - path); dir[name - path] = 0;   /* without the last '/' ("" = the root) */
    name++;
    mutex_lock(&aa_mtx);
    i = aa_dir(dir);
    if (i < 0 || aa_c[i].overflow)
    {
        mutex_unlock(&aa_mtx);
        return aa_fe(path);
    }
    for (k = 0; k < aa_c[i].used; k += strlen(aa_c[i].pool + k) + 1)
        if (!strcasecmp(aa_c[i].pool + k, name)) { found = true; break; }
    mutex_unlock(&aa_mtx);
    return found;
}
#undef file_exists
#define file_exists(p) aa_exists(p)
#endif

#ifdef USE_JPEG_COVER
static const char * const extensions[] = { "jpeg", "jpg", "bmp" };
static const unsigned char extension_lens[] = { 4, 3, 3 };
/* Try checking for several file extensions, return true if a file is found and
 * leaving the path modified to include the matching extension.
 */
static bool try_exts(char *path, int len)
{
    int i;
    for (i = 0; i < 3; i++)
    {
        if (extension_lens[i] + len > MAX_PATH)
            continue;
        strcpy(path + len, extensions[i]);
        if (file_exists(path))
            return true;
    }
    return false;
}
#define EXT
#else
#define EXT "bmp"
#define try_exts(path, len) file_exists(path)
#endif

/* Look for the first matching album art bitmap in the following list:
 *  ./<trackname><size>.{jpeg,jpg,bmp}
 *  ./<albumname><size>.{jpeg,jpg,bmp}
 *  ./cover<size>.bmp
 *  ../<albumname><size>.{jpeg,jpg,bmp}
 *  ../cover<size>.{jpeg,jpg,bmp}
 *  ROCKBOX_DIR/albumart/<artist>-<albumname><size>.{jpeg,jpg,bmp}
 * <size> is the value of the size_string parameter, <trackname> and
 * <albumname> are read from the ID3 metadata.
 * If a matching bitmap is found, its filename is stored in buf.
 * Return value is true if a bitmap was found, false otherwise.
 *
 * If the first symbol in size_string is a colon (e.g. ":100x100")
 * then the colon is skipped ("100x100" will be used) and the track
 * specific image (./<trackname><size>.bmp) is tried last instead of first.
 */
bool search_albumart_files(const struct mp3entry *id3, const char *size_string,
                           char *buf, int buflen)
{
    char path[MAX_PATH + 11]; /* need room for filename and null termination */
    char dir[MAX_PATH + 1];
    bool found = false;
    int track_first = 1;
    int pass;
    const char *trackname;
    const char *artist;
    int dirlen;
    int albumlen;
    int pathlen;

    if (!id3 || !buf)
        return false;

    trackname = id3->path;

    if (strcmp(trackname, "No file!") == 0)
        return false;

    if (*size_string == ':')
    {
        size_string++;
        track_first = 0;
    }

    strip_filename(dir, sizeof(dir), trackname);
    dirlen = strlen(dir);
    albumlen = id3->album ? strlen(id3->album) : 0;

    for(pass = 0; pass < 2 - track_first; pass++)
    {
        if (track_first || pass)
        {
            /* the first file we look for is one specific to the
               current track */
            strip_extension(path, sizeof(path) - strlen(size_string) - 4,
                            trackname);
            strcat(path, size_string);
            strcat(path, "." EXT);
#ifdef USE_JPEG_COVER
            pathlen = strlen(path);
#endif
            found = try_exts(path, pathlen);
        }
        if (pass)
            break;
        if (!found && albumlen > 0)
        {
            /* if it doesn't exist,
            * we look for a file specific to the track's album name */
            pathlen = snprintf(path, sizeof(path),
                            "%s%s%s." EXT, dir, id3->album, size_string);
            fix_path_part(path, dirlen, albumlen);
            found = try_exts(path, pathlen);
        }

        if (!found)
        {
            /* if it still doesn't exist, we look for a generic file */
            pathlen = snprintf(path, sizeof(path),
                            "%scover%s." EXT, dir, size_string);
            found = try_exts(path, pathlen);
        }

#ifdef USE_JPEG_COVER
        if (!found && !*size_string)
        {
            snprintf (path, sizeof(path), "%sfolder.jpg", dir);
            found = file_exists(path);
        }
#endif

        artist = id3->albumartist != NULL ? id3->albumartist : id3->artist;

        if (!found && artist && id3->album)
        {
            /* look in the albumart subdir of .rockbox */
            pathlen = snprintf(path, sizeof(path),
                            ROCKBOX_DIR "/albumart/%s-%s%s." EXT,
                            artist,
                            id3->album,
                            size_string);
            fix_path_part(path, strlen(ROCKBOX_DIR "/albumart/"), MAX_PATH);
            found = try_exts(path, pathlen);
        }

        if (!found)
        {
            /* if it still doesn't exist,
            * we continue to search in the parent directory */
            strcpy(path, dir);
            path[dirlen - 1] = 0;
            strip_filename(dir, sizeof(dir), path);
            dirlen = strlen(dir);
        }

        /* only try parent if there is one */
        if (dirlen > 0)
        {
            if (!found && albumlen > 0)
            {
                /* we look in the parent directory
                * for a file specific to the track's album name */
                pathlen = snprintf(path, sizeof(path),
                                "%s%s%s." EXT, dir, id3->album, size_string);
                fix_path_part(path, dirlen, albumlen);
                found = try_exts(path, pathlen);
            }

            if (!found)
            {
                /* if it still doesn't exist, we look in the parent directory
                * for a generic file */
                pathlen = snprintf(path, sizeof(path),
                                "%scover%s." EXT, dir, size_string);
                found = try_exts(path, pathlen);
            }
        }
        if (found)
            break;
    }

    if (!found)
        return false;

    strmemccpy(buf, path, buflen);
    logf("Album art found: %s", path);
    return true;
}

#ifndef PLUGIN
/* Look for albumart bitmap in the same dir as the track and in its parent dir.
 * Stores the found filename in the buf parameter.
 * Returns true if a bitmap was found, false otherwise */
bool find_albumart(const struct mp3entry *id3, char *buf, int buflen,
                    const struct dim *dim)
{
    if (!id3 || !buf)
        return false;

    char size_string[15];/* .-32768x-32768\0 */
    logf("Looking for album art for %s", id3->path);

    /* Write the size string, e.g. ".100x100". */
    snprintf(size_string, sizeof(size_string), ".%dx%d",
              dim->width, dim->height);

#if !defined(SIMULATOR) && defined(IPOD_VIDEO)
    {   /* bcm overlay v139: Debug Log on -> /.rockbox/play_timing.txt gets one line per search */
        unsigned long t0 = USEC_TIMER;
        unsigned l0 = aa_lookups, s0 = aa_scans;
        bool r = search_albumart_files(id3, size_string, buf, buflen);
        if (!r)
        {
            *size_string = 0;
            r = search_albumart_files(id3, size_string, buf, buflen);
        }
        if (global_settings.bcm_debug_log)
        {
            int fd = open("/.rockbox/play_timing.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (fd >= 0)
            {
                fdprintf(fd, "%ld.%02ld album art search %lu ms, %u look-ups, %u folder reads, %s  %s\n",
                         current_tick / HZ, current_tick % HZ, (unsigned long)(USEC_TIMER - t0) / 1000,
                         aa_lookups - l0, aa_scans - s0, r ? "found" : "none", id3->path);
                close(fd);
            }
        }
        return r;
    }
#else
    /* First we look for a bitmap of the right size */
    if (search_albumart_files(id3, size_string, buf, buflen))
        return true;

    /* Then we look for generic bitmaps */
    *size_string = 0;
    return search_albumart_files(id3, size_string, buf, buflen);
#endif
}

#endif /* PLUGIN */
