/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * BCM2722 host-interface bus I/O hooks.
 * Swapped in by the PC test harness; on the iPod these are real MMIO accesses.
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
#ifndef BCM_HOST_IO_H
#define BCM_HOST_IO_H

#ifdef BCM_HOST_TEST
/* PC test shim — implemented in bcm_host_test.c */
#include <stdint.h>
void     bcm_io_write(uint32_t addr, uint32_t val);
uint32_t bcm_io_read(uint32_t addr);
void     bcm_io_lock(void);
void     bcm_io_unlock(void);
void     bcm_io_doorbell(void);
void     bcm_io_sleep_ticks(int ticks);
uint32_t bcm_io_current_tick(void);
#else
/* Real iPod hardware */
#include "config.h"
#include "system.h"
#include "kernel.h"

/* BCM bus register physical addresses */
#define BCM_DATA        0x30000000u  /* 32-bit data window into VC SRAM      */
#define BCM_WR_ADDR32   0x30010000u  /* write pointer (VC word address)       */
#define BCM_RD_ADDR32   0x30020000u  /* read pointer  (VC word address)       */
#define BCM_CONTROL     0x30030000u  /* write 0x31 to ring the doorbell        */
#define BCM_ALT0        0x30040000u
#define BCM_ALT1        0x30050000u
#define BCM_ALT2        0x30060000u
#define BCM_ALT3        0x30070000u

/* GPO bit 14 controls BCM power rail */
#define BCM_POWER_BIT   (1u << 14)

static inline void bcm_io_write(uint32_t addr, uint32_t val)
{
    REG32(addr) = val;
}
static inline uint32_t bcm_io_read(uint32_t addr)
{
    return REG32(addr);
}

/* The bus mutex lives in lcd-video.c; declared there, used here */
extern struct mutex bcm_bus_mutex;

static inline void bcm_io_lock(void)    { mutex_lock(&bcm_bus_mutex); }
static inline void bcm_io_unlock(void)  { mutex_unlock(&bcm_bus_mutex); }

static inline void bcm_io_doorbell(void)
{
    bcm_io_write(BCM_CONTROL, 0x31);
}
static inline void bcm_io_sleep_ticks(int ticks)
{
    sleep(ticks);
}
static inline uint32_t bcm_io_current_tick(void)
{
    return (uint32_t)current_tick;
}
#endif /* BCM_HOST_TEST */

#endif /* BCM_HOST_IO_H */
