/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
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
/* bcm overlay v125: the CABAC -> CAVLC transcoder again, linked to run from IRAM.
 *
 * The v124 device logs: the SDRAM copy runs 2-2.5x slower than the cycle model (46 KB of -O3 code against the PP5022's
 * small cache).  Every function and table of this copy goes into the .c2ciram sections, which app-pp.lds places at
 * 0x4000c000 -- the IRAM Rockbox's codecs and plugins use -- with the load image kept in SDRAM.  debug_menu.c copies it
 * in (c2c_iram_claim) only while nothing else can use that IRAM: Rockbox's playback stopped (the VideoCore plays the
 * sound), and it switches back to the SDRAM copy before Rockbox's own sound starts. */
#define C2C_TEXT   __attribute__((section(".c2ciram.text")))
#define C2C_RODATA __attribute__((section(".c2ciram.rodata")))
#define C2C_BSS    __attribute__((section(".c2ciram.bss")))
#define c2c_state_size c2ci_state_size
#define c2c_init       c2ci_init
#define c2c_nal        c2ci_nal
#define c2c_stats      c2ci_stats
#define c2c_errstr     c2ci_errstr
#define c2c_set_escape c2ci_set_escape
#include "bcm_cab2cav.c"
