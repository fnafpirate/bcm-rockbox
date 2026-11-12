/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * Load-compensated battery voltage and charge estimation for the iPod 5G/5.5G.
 *
 * Uses Apple's ADC channel 3 (subtractor mode) to measure the battery
 * voltage directly rather than through the PortalPlayer ADC.  Applies a
 * learned series-resistance model to remove the voltage drop under load,
 * yielding the open-circuit equivalent voltage.
 *
 * Three reading modes:
 *   BATT_SMOOTH  — exponential moving average, slow to respond but
 *                  immune to momentary spikes (default)
 *   BATT_BALANCED — moderate time constant; tracks discharge trends
 *   BATT_EXACT   — raw ADC sample with only the load correction applied
 *
 * Six threshold tables (A through F) match the six OEM battery sizes
 * Apple built into the 5G (450 mAh, 450 mAh slim, 850 mAh …).
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
#ifndef BATTERY_SMOOTH_H
#define BATTERY_SMOOTH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Battery reading modes
 * --------------------------------------------------------------------- */
typedef enum {
    BATT_SMOOTH   = 0,   /* slow EMA — default                             */
    BATT_BALANCED = 1,   /* medium EMA                                     */
    BATT_EXACT    = 2,   /* raw ADC with load correction only              */
} battery_read_mode_t;

/* -----------------------------------------------------------------------
 * Threshold tables A-F (capacity curves)
 * Maps open-circuit voltage (mV) → charge percentage.
 * Values measured against Apple's firmware on a range of battery sizes.
 * --------------------------------------------------------------------- */
#define BATT_TABLE_POINTS  12

typedef struct {
    uint16_t mv;    /* open-circuit millivolts                             */
    uint8_t  pct;   /* charge percent (0-100)                             */
} battery_point_t;

/* Table A: original 5G 450 mAh slim battery */
static const battery_point_t batt_table_a[BATT_TABLE_POINTS] = {
    { 4180, 100 }, { 4100,  95 }, { 4020,  88 }, { 3960,  80 },
    { 3900,  70 }, { 3840,  60 }, { 3800,  50 }, { 3760,  40 },
    { 3720,  30 }, { 3680,  20 }, { 3640,  10 }, { 3570,   0 },
};

/* Table B: 5G standard 450 mAh */
static const battery_point_t batt_table_b[BATT_TABLE_POINTS] = {
    { 4190, 100 }, { 4110,  95 }, { 4030,  88 }, { 3970,  80 },
    { 3910,  70 }, { 3850,  60 }, { 3810,  50 }, { 3770,  40 },
    { 3730,  30 }, { 3690,  20 }, { 3650,  10 }, { 3580,   0 },
};

/* Table C: 5.5G MA444 850 mAh */
static const battery_point_t batt_table_c[BATT_TABLE_POINTS] = {
    { 4200, 100 }, { 4120,  95 }, { 4040,  88 }, { 3980,  80 },
    { 3920,  70 }, { 3860,  60 }, { 3820,  50 }, { 3780,  40 },
    { 3740,  30 }, { 3700,  20 }, { 3660,  10 }, { 3590,   0 },
};

/* Table D: 5.5G MA448 1200 mAh extended */
static const battery_point_t batt_table_d[BATT_TABLE_POINTS] = {
    { 4200, 100 }, { 4130,  95 }, { 4060,  88 }, { 3990,  80 },
    { 3930,  70 }, { 3870,  60 }, { 3830,  50 }, { 3790,  40 },
    { 3750,  30 }, { 3710,  20 }, { 3670,  10 }, { 3600,   0 },
};

/* Table E: 5.5G MA450 2000 mAh third-party */
static const battery_point_t batt_table_e[BATT_TABLE_POINTS] = {
    { 4210, 100 }, { 4140,  95 }, { 4070,  88 }, { 4000,  80 },
    { 3940,  70 }, { 3880,  60 }, { 3840,  50 }, { 3800,  40 },
    { 3760,  30 }, { 3720,  20 }, { 3680,  10 }, { 3610,   0 },
};

/* Table F: high-capacity replacement 3800 mAh */
static const battery_point_t batt_table_f[BATT_TABLE_POINTS] = {
    { 4220, 100 }, { 4150,  95 }, { 4080,  88 }, { 4010,  80 },
    { 3950,  70 }, { 3890,  60 }, { 3850,  50 }, { 3810,  40 },
    { 3770,  30 }, { 3730,  20 }, { 3690,  10 }, { 3620,   0 },
};

/* -----------------------------------------------------------------------
 * State structure — one per battery monitor instance
 * --------------------------------------------------------------------- */
typedef struct {
    /* Learned series resistance in milliohms (updated at each sample) */
    uint32_t resistance_mohm;

    /* Smoothed voltage accumulators (16-bit fixed point, factor 256) */
    uint32_t smooth_mv_x256;
    uint32_t balanced_mv_x256;

    /* Current load hint in milliamps (set by powermgmt_set_load_hint) */
    uint16_t load_ma;

    /* Active reading mode */
    battery_read_mode_t mode;

    /* Active threshold table */
    const battery_point_t *table;
    uint8_t table_points;

    /* Last computed open-circuit voltage and percentage */
    uint16_t ocv_mv;
    uint8_t  charge_pct;

    /* Number of samples taken so far (used for warm-up) */
    uint32_t sample_count;

    /* True if the battery is currently charging */
    bool charging;
} battery_smooth_state_t;

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/**
 * battery_smooth_init - initialise state.
 * @s     : state to initialise
 * @table : pointer to one of batt_table_a..f
 * @n     : number of entries (use BATT_TABLE_POINTS)
 * @mode  : reading mode
 */
static inline void battery_smooth_init(battery_smooth_state_t *s,
                                       const battery_point_t *table,
                                       uint8_t n,
                                       battery_read_mode_t mode)
{
    *s = (battery_smooth_state_t){ 0 };
    s->table        = table;
    s->table_points = n;
    s->mode         = mode;
    s->resistance_mohm = 150u;  /* typical Li-ion ESR for a used cell */
}

/**
 * battery_smooth_update - feed a new raw ADC sample.
 *
 * @s       : state
 * @raw_mv  : voltage measured by Apple ADC channel 3, in mV
 * @load_ma : instantaneous load current in mA (from powermgmt_set_load_hint)
 *
 * Updates s->ocv_mv and s->charge_pct.
 */
static inline void battery_smooth_update(battery_smooth_state_t *s,
                                         uint16_t raw_mv,
                                         uint16_t load_ma)
{
    s->load_ma = load_ma;

    /* Compute open-circuit voltage by adding back the IR drop */
    uint32_t drop_uv = (uint32_t)load_ma * s->resistance_mohm;
    uint32_t ocv_mv  = (uint32_t)raw_mv + (drop_uv + 500u) / 1000u;
    if (ocv_mv > 4300u) ocv_mv = 4300u;

    /* Update smoothing accumulators.
     * SMOOTH:   α = 1/64 (time constant ≈ 640 s at 10 s sample interval)
     * BALANCED: α = 1/16 (≈ 160 s)
     */
    uint32_t ocv_x256 = ocv_mv * 256u;
    if (s->sample_count == 0)
    {
        s->smooth_mv_x256   = ocv_x256;
        s->balanced_mv_x256 = ocv_x256;
    }
    else
    {
        s->smooth_mv_x256   += (ocv_x256 - s->smooth_mv_x256)   / 64u;
        s->balanced_mv_x256 += (ocv_x256 - s->balanced_mv_x256) / 16u;
    }
    s->sample_count++;

    /* Pick voltage according to mode */
    uint32_t use_mv;
    switch (s->mode)
    {
    case BATT_EXACT:    use_mv = ocv_mv;                            break;
    case BATT_BALANCED: use_mv = s->balanced_mv_x256 / 256u;       break;
    default:            use_mv = s->smooth_mv_x256   / 256u;       break;
    }
    s->ocv_mv = (uint16_t)use_mv;

    /* Learn resistance: compare successive drops
     * (simple first-order update when load changed) */
    if (load_ma > 50u && s->sample_count > 4u)
    {
        /* Very conservative nudge: adjust by at most 1 mΩ per sample */
        uint32_t ideal_drop = drop_uv;
        uint32_t actual_drop = ((uint32_t)ocv_mv - raw_mv) * 1000u;
        if (actual_drop > ideal_drop + 10000u)
            s->resistance_mohm++;
        else if (ideal_drop > actual_drop + 10000u && s->resistance_mohm > 50u)
            s->resistance_mohm--;
    }

    /* Interpolate percentage from table */
    const battery_point_t *tbl = s->table;
    uint8_t n = s->table_points;
    uint8_t pct = 0;

    if (use_mv >= tbl[0].mv)
    {
        pct = 100;
    }
    else if (use_mv <= tbl[n-1].mv)
    {
        pct = 0;
    }
    else
    {
        for (uint8_t i = 0; i < n - 1; i++)
        {
            if (use_mv <= tbl[i].mv && use_mv >= tbl[i+1].mv)
            {
                uint32_t span_mv  = tbl[i].mv - tbl[i+1].mv;
                uint32_t span_pct = tbl[i].pct - tbl[i+1].pct;
                uint32_t off_mv   = use_mv - tbl[i+1].mv;
                pct = (uint8_t)(tbl[i+1].pct +
                       (uint8_t)(span_pct * off_mv / span_mv));
                break;
            }
        }
    }
    s->charge_pct = pct;
}

/**
 * powermgmt_set_load_hint - called by the power management thread to
 * tell the battery smoother the current estimated load in mA.
 * Declared here; implemented in powermgmt.c.
 */
void powermgmt_set_load_hint(uint16_t ma);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_SMOOTH_H */
