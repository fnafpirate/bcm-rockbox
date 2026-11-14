/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 by Michael Sevakis
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
#ifndef FS_DEFINES_H
#define FS_DEFINES_H

/** Tuneable parameters **/

#if 0
/* Define this just in case you're doing something that may crash a lot and
   want less write caching */
#define FS_MIN_WRITECACHING
#endif

#ifndef MAX_PATH
#define MAX_PATH        260
#endif

#define MAX_COMPNAME    260

/* limits for number of open descriptors - if you increase these values, make
   certain that the disk cache has enough available buffers */

#if MEMORYSIZE < 8
#define MAX_OPEN_FILES  11
#define MAX_OPEN_DIRS   12
#else
#define MAX_OPEN_FILES  31
#define MAX_OPEN_DIRS   32
#endif /* MEMORYSIZE */


/* internal functions open streams as well; make sure they don't fail if all
   user descs are busy; this needs to be at least the greatest quantity needed
   at once by all internal functions */
#define MOUNT_AUX_FILEOBJS 1
#ifdef HAVE_DIRCACHE
#define DIRCACHE_AUX_FILEOBJS 1
#else
#define DIRCACHE_AUX_FILEOBJS 0
#endif
#define AUX_FILEOBJS (2+DIRCACHE_AUX_FILEOBJS+MOUNT_AUX_FILEOBJS)

/* number of components statically allocated to handle the vast majority
   of path depths; should maybe be tuned for >= 90th percentile but for now,
   imma just guessing based on something like:
        root + 'Music' + 'Artist' + 'Album' + 'Disc N' + filename */
#define STATIC_PATHCOMP_NUM 6

/* unsigned value that will also hold the off_t range we need without
   overflow.  bcm overlay v89: 64-bit -- exFAT files bigger than 4 GiB keep their real size and are fully readable
   and writable through lseek64() / ffilesize64(); the classic off_t calls below keep their 2 GiB range */
#ifdef BOOTLOADER
#define file_size_t uint32_t   /* v89: the bootloader keeps v88's sizes (byte-identical; it only loads rockbox.ipod) */
#else
#define file_size_t uint64_t
#endif

#ifdef __USE_FILE_OFFSET64
/* if we want, we can deal with files up to 2^32-1 bytes-- the full FAT16/32
   range */
#define FILE_SIZE_MAX   (0xffffffffu)
#else
/* bcm overlay v89: now only the range of the 32-bit off_t calls (lseek, ffilesize, ftruncate); read() and write()
   go on past it (sequentially or after lseek64()) up to what the volume allows: 4 GiB - 1 on FAT32, 2 TiB (with
   512-byte sectors) on exFAT -- fat_file_max_size() */
#define FILE_SIZE_MAX   (0x7fffffffu)
#endif

/* if file is "large(ish)", then get rid of the contents now rather than
   lazily when the file is synced or closed in order to free-up space */
#define O_TRUNC_THRESH 65536

/* This needs enough for all file handles to have a buffer in the worst case
 * plus at least one reserved exclusively for the cache client and a couple
 * for other file system code. The buffers are put to use by the cache if not
 * taken for another purpose (meaning nothing is wasted sitting fallow).
 *
 * One map per volume is maintained in order to avoid collisions between
 * volumes that would slow cache probing. IOC_MAP_NUM_ENTRIES is the number
 * for each map per volume. The buffers themselves are shared.
 */
#if MEMORYSIZE < 8
#define DC_NUM_ENTRIES      32
#define DC_MAP_NUM_ENTRIES  128
#elif MEMORYSIZE >= 32 && !defined(BOOTLOADER)
/* bcm overlay v113: 256 sectors (128 KB) on the 32 / 64 MB iPods: a directory of a few hundred entries, the parent
   directories, the FAT and bitmap sectors in use and the open files' buffers all fit at once (64 = 32 KB did not: every
   create / rename / delete in a 100-file folder re-read the folder).  The boot loader keeps 64. */
#define DC_NUM_ENTRIES      256
#define DC_MAP_NUM_ENTRIES  1024
#else
#define DC_NUM_ENTRIES      64
#define DC_MAP_NUM_ENTRIES  256
#endif /* MEMORYSIZE */

/* increasing this will increase the total memory used by the cache; the
   cache, as noted in disk_cache.h, has other minimum requirements that may
   prevent reducing its number of entries in order to compensate */
#ifndef SECTOR_SIZE
#define SECTOR_SIZE     512
#endif

/* this _could_ be larger than a sector if that would ever be useful */
#ifdef MAX_VARIABLE_LOG_SECTOR
#define DC_CACHE_BUFSIZE   MAX_VARIABLE_LOG_SECTOR
#else
#define DC_CACHE_BUFSIZE    SECTOR_SIZE
#endif

#endif /* FS_DEFINES_H */
