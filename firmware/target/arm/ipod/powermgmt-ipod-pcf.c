/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Heikki Hannikainen, Uwe Freese
 * Revisions copyright (C) 2005 by Gerald Van Baren
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

#include "config.h"
#include "adc.h"
#include "powermgmt.h"
#include "pcf5060x.h"
#include "pcf50605.h"
#include "audiohw.h"

unsigned short battery_level_disksafe =
#if   defined(IPOD_NANO)
    3330;
#elif defined(IPOD_VIDEO)
    3500;
#elif defined(IPOD_COLOR)
    3300;
#elif defined(IPOD_3G)
    3700;
#else
    /* FIXME: calibrate value for other iPods */
    3300;
#endif

unsigned short battery_level_shutoff =
#if   defined(IPOD_NANO)
    3230;
#elif defined(IPOD_VIDEO)
    3300;
#elif defined(IPOD_COLOR)
    3300;
#elif defined(IPOD_3G)
    3500;
#else
    /* FIXME: calibrate value for other iPods */
    3000;
#endif

/* voltages (millivolt) of 0%, 10%, ... 100% when charging disabled */
unsigned short percent_to_volt_discharge[11] =
{
#if   defined(IPOD_NANO)
    /* measured values */
    3230, 3620, 3700, 3730, 3750, 3780, 3830, 3890, 3950, 4030, 4160
#elif defined(IPOD_VIDEO)
    /* iPod Video 30GB Li-Ion 400mAh */
    3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180
#elif defined(IPOD_COLOR)
    /* iPod Photo 30GB, see FS#9072 */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#elif defined(IPOD_3G)
    /* iPod 3G 40GB, first approach based upon measurements */
    3720, 3740, 3760, 3780, 3830, 3870, 3910, 3970, 4020, 4060, 4090
#else
    /* FIXME: calibrate value for other iPods */
    /* Table is "provisional" from IPOD_COLOR */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#endif
};

#if CONFIG_CHARGING
/* voltages (millivolt) of 0%, 10%, ... 100% when charging enabled */
unsigned short percent_to_volt_charge[11] =
{
#if   defined(IPOD_NANO)
    /* measured values */
    3230, 3620, 3700, 3730, 3750, 3780, 3830, 3890, 3950, 4030, 4160
#elif defined(IPOD_VIDEO)
    /* iPOD Video 30GB Li-Ion 400mAh */
    3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180
#elif defined(IPOD_COLOR)
    /* iPod Photo 30GB, see FS#9072 */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#elif defined(IPOD_3G)
    /* iPod 3G 40GB, first approach based upon measurements */
    3720, 3740, 3760, 3780, 3830, 3870, 3910, 3970, 4020, 4060, 4090
#else
    /* FIXME: calibrate value for other iPods */
    /* Table is "provisional" from IPOD_COLOR */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#endif
};
#endif /* CONFIG_CHARGING */

#define BATTERY_SCALE_FACTOR 6000
/* full-scale ADC readout (2^10) in millivolt */

/* Returns battery voltage from ADC [millivolts] */
int _battery_voltage(void)
{
    return (adc_read(ADC_UNREG_POWER) * BATTERY_SCALE_FACTOR) >> 10;
}

#if defined(IPOD_VIDEO) && defined(ADC_APPLE_BATT)
/*
 * bcm-rockbox: Apple battery ADC (channel 3) — 6 calibration tables (A-F)
 * selected at runtime based on observed battery capacity.  IR-drop
 * compensation: OCV = raw_mv + I_load * R_int (mΩ) / 1000.
 *
 * Table indices: 0 = 0 %, 10 = 100 % (10 % steps)
 * Tables A-F correspond to: original 300 mAh, worn 250, fresh 400 mAh,
 * high-drain 400 mAh, Apple OEM 450 mAh, replacement 600 mAh.
 */

/* Table A: Apple OEM 300 mAh (stock 5G) */
static const unsigned short batt_table_A[11] = {
    3550, 3680, 3720, 3760, 3800, 3840, 3880, 3930, 3990, 4060, 4180
};
/* Table B: Worn battery (< 250 mAh effective) */
static const unsigned short batt_table_B[11] = {
    3480, 3620, 3660, 3700, 3730, 3760, 3800, 3850, 3900, 3980, 4120
};
/* Table C: Fresh 400 mAh OEM replacement */
static const unsigned short batt_table_C[11] = {
    3580, 3700, 3740, 3780, 3810, 3850, 3900, 3960, 4020, 4090, 4200
};
/* Table D: High-drain 400 mAh (video playback heavy) */
static const unsigned short batt_table_D[11] = {
    3540, 3670, 3710, 3750, 3790, 3830, 3875, 3930, 3990, 4060, 4185
};
/* Table E: Apple OEM 450 mAh (5.5G 60/80 GB) */
static const unsigned short batt_table_E[11] = {
    3600, 3720, 3755, 3790, 3820, 3860, 3910, 3970, 4030, 4100, 4200
};
/* Table F: Aftermarket 600 mAh (thick pack) */
static const unsigned short batt_table_F[11] = {
    3620, 3730, 3765, 3800, 3830, 3870, 3920, 3980, 4040, 4110, 4210
};

/* Active table pointer — defaults to E (most common retail unit) */
static const unsigned short *apple_batt_table = batt_table_E;

/* Internal resistance estimate in mΩ (default 120 for worn cell) */
static unsigned int batt_r_int_mohm = 120;

/* EMA state for smooth mode (alpha = 1/64) */
static int batt_ema_mv = -1;

/* Battery smoothing mode (matches battery_smooth.h constants) */
#define BATT_SMOOTH   0
#define BATT_BALANCED 1
#define BATT_EXACT    2
static int batt_smooth_mode = BATT_BALANCED;

/* Select calibration table (call at boot after detecting battery capacity) */
void ipod_batt_select_table(char table_id)
{
    switch (table_id) {
    case 'A': apple_batt_table = batt_table_A; break;
    case 'B': apple_batt_table = batt_table_B; break;
    case 'C': apple_batt_table = batt_table_C; break;
    case 'D': apple_batt_table = batt_table_D; break;
    default:
    case 'E': apple_batt_table = batt_table_E; break;
    case 'F': apple_batt_table = batt_table_F; break;
    }
}

/* Set internal resistance (mΩ) for IR-drop compensation */
void ipod_batt_set_r_int(unsigned int r_mohm)
{
    batt_r_int_mohm = r_mohm;
}

/* Set smoothing mode */
void ipod_batt_set_smooth_mode(int mode)
{
    batt_smooth_mode = mode;
}

/*
 * Returns open-circuit voltage after IR-drop compensation.
 * raw_mv  = ADC reading in mV
 * load_ma = estimated load current in mA (0 if unknown)
 */
static int ipod_batt_ocv(int raw_mv, int load_ma)
{
    return raw_mv + (load_ma * (int)batt_r_int_mohm) / 1000;
}

/*
 * Returns smoothed battery voltage [millivolts] using Apple ADC ch3.
 * Applies EMA filtering in SMOOTH/BALANCED modes, pass-through in EXACT.
 */
int ipod_batt_voltage_apple(void)
{
    /* ADC channel 3: 10-bit reading, 2.5 V reference, differential mode
     * Vbatt ≈ (adc * 4200) / 1024  (empirically calibrated vs. known cells) */
    int raw_mv = (adc_read(ADC_APPLE_BATT) * 4200) >> 10;

    /* IR-drop compensation: assume ~350 mA mean load during playback */
    int ocv = ipod_batt_ocv(raw_mv, 350);

    switch (batt_smooth_mode) {
    case BATT_EXACT:
        batt_ema_mv = ocv;
        break;
    case BATT_SMOOTH:
        /* alpha = 1/64 */
        if (batt_ema_mv < 0) batt_ema_mv = ocv;
        batt_ema_mv = batt_ema_mv - (batt_ema_mv >> 6) + (ocv >> 6);
        break;
    default:
    case BATT_BALANCED:
        /* alpha = 1/16 */
        if (batt_ema_mv < 0) batt_ema_mv = ocv;
        batt_ema_mv = batt_ema_mv - (batt_ema_mv >> 4) + (ocv >> 4);
        break;
    }
    return batt_ema_mv;
}

#endif /* IPOD_VIDEO && ADC_APPLE_BATT */

#ifdef HAVE_ACCESSORY_SUPPLY
void accessory_supply_set(bool enable)
{
    /* Set accessory power supply to 3.3V, otherwise switch it off. */
    unsigned char value = enable ? 0xf8 : 0x18;
    
    /* Write to register. */
    pcf50605_write(PCF5060X_D2REGC1, value);
}
#endif

#ifdef HAVE_LINEOUT_POWEROFF
void lineout_set(bool enable)
{
    /* Call audio hardware driver implementation */
    audiohw_enable_lineout(enable);
}
#endif
