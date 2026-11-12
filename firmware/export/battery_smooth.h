/***************************************************************************
 * Smoothed ("stock-like") battery percentage for voltage-only targets.
 *
 * Rockbox shows the battery level straight from a ~1 minute average of the
 * battery voltage.  On the iPod Video one percent is only ~3 mV in the middle
 * of the curve, so the voltage drop caused by the backlight, the CPU boost,
 * playback or the VideoCore decoding a video (tens of mV through the cell's
 * internal resistance) moves the reading by 5-15 % and it bounces up and down
 * with every load change.  The stock firmware shows a calm value instead.
 *
 * This keeps Rockbox's exact voltage-based number but:
 *  1. learns, on this iPod and battery, how many mV each known load pulls the
 *     voltage down (watching the voltage step whenever exactly one load turns
 *     on or off) and adds that back ("load compensation"),
 *  2. filters the compensated voltage slowly (tau 45 s),
 *  3. shows a percentage that only goes DOWN while running on battery, one
 *     step at a time and only after the estimate has stayed below it for a
 *     while (faster when the gap is large), and only goes back UP if the
 *     estimate stays clearly higher for minutes (e.g. after a long heavy load),
 *  4. while charging counts up at the estimated charge rate (the voltage reads
 *     high while charging) and goes to 100 % when the charger reports full,
 *     and holds the value for 2 minutes after the charger is unplugged.
 * Low-battery protection is untouched: it still uses the raw voltage.
 *
 * bcm overlay v92: "balanced" (the default) is a mix of the two: it follows
 * the battery both ways like Rockbox's own reading, at about stock speed,
 * but every sample first goes through a spike filter (median of the last 5,
 * so a disk spin-up, the VideoCore starting or a contact bounce never gets
 * in), the learned load compensation stays, and the shown number only moves
 * once the estimate has been clearly past it for a little while (10 s down,
 * 30 s up), 1 % per 2 s when it is far off.  "smooth" is the v58 behaviour.
 *
 * Pure C, no Rockbox dependencies: also compiled by the host simulation in
 * tools/bcm/test/battery_smooth_test.c.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#ifndef BATTERY_SMOOTH_H
#define BATTERY_SMOOTH_H

#include <stdint.h>
#include <stdbool.h>

/* load classes (bit numbers) */
enum { BSM_BACKLIGHT = 0, BSM_BOOST, BSM_AUDIO, BSM_VIDEO, BSM_NCLS };

/* rough extra current of each load on the iPod Video (mA); a load's voltage drop = the cell's resistance x this until
   that load's own drop has been measured 3 times.  The resistance is learned from every measured step (mostly the
   backlight and the CPU boost, which switch often), so a load that rarely switches cleanly (the video decoder) still
   gets this battery's resistance. */
#define BSM_LOAD_MA      { 30, 20, 15, 140 }
#define BSM_R_DEFAULT    250       /* milliohm */
#define BSM_SAG_MAX      250

struct bsm {
    bool    inited;
    bool    balanced;     /* v92: the mixed mode (set by the caller) */
    int16_t med[5];       /* v92: the last raw samples (spike filter) */
    uint8_t med_n, med_i;
    int32_t vfast;        /* measured mV, EMA tau ~4 s, x256 */
    int32_t vslow;        /* compensated mV, EMA tau ~45 s, x256 */
    int16_t sag[BSM_NCLS];         /* in use (mV) */
    int16_t meas[BSM_NCLS];        /* measured directly (mV) */
    int16_t learned[BSM_NCLS];     /* how many times each sag was measured */
    int32_t r_mohm;                /* learned internal resistance */
    int32_t r_n;
    int32_t vlearn;                /* measured mV, EMA tau 2 s, x256 (step measurements) */
    unsigned cls;         /* loads active now */
    int32_t stable_ms;    /* time since the last load change */
    int     pend_bit;     /* -1 or the load whose step is being measured */
    bool    pend_on;
    int32_t pend_v0;      /* vfast just before the step, x256 */
    int32_t pend_ms;
    /* the video decoder rarely switches alone (CPU boost, backlight and the disk change with it): its step is measured
       over 30 s on the compensated voltage of everything else, gaps allowed */
    bool    vpend, vpend_on;
    int32_t vpend_c0, vpend_ms;
    int     display;      /* the shown percentage */
    int     target_pm;    /* estimate in per mille (for debug) */
    int     raw_pm;       /* uncompensated estimate in per mille (for debug) */
    int32_t below_ms, above_ms, hold_ms;
    int32_t chg_acc;      /* charging: mAh*3600/1000 accumulator ("mA*ms") */
    int     mode;         /* 0 battery, 1 charging, 2 charged (charger still in), 3 powered, not charging */
};

/* per mille from a 0..100 % table of 11 voltages */
static inline int bsm_pm(int mv, const unsigned short *t)
{
    int i = 0;
    if (mv <= t[0]) return 0;
    if (mv >= t[10]) return 1000;
    while (i < 10 && t[i + 1] < mv) i++;
    return i * 100 + (mv - t[i]) * 100 / (t[i + 1] - t[i]);
}

static inline int bsm_comp(const struct bsm *s, int32_t vfast)
{
    int k, mv = (int)(vfast >> 8);
    for (k = 0; k < BSM_NCLS; k++)
        if (s->cls & (1u << k)) mv += s->sag[k];
    return mv;
}

static inline void bsm_update_sags(struct bsm *s)
{
    static const int16_t ma[BSM_NCLS] = BSM_LOAD_MA;
    int k;
    for (k = 0; k < BSM_NCLS; k++)
        s->sag[k] = s->learned[k] >= (k == BSM_VIDEO ? 1 : 3) ? s->meas[k] : (int16_t)(s->r_mohm * ma[k] / 1000);
}

/* v92: median of the last 5 raw samples -- one or two wild samples (a spin-up, a load switching mid-reading) never
   reach the filters */
static inline int bsm_median(struct bsm *s, int mv)
{
    int a[5], n, i, j;
    s->med[s->med_i] = (int16_t)mv;
    s->med_i = (uint8_t)((s->med_i + 1) % 5);
    if (s->med_n < 5) s->med_n++;
    n = s->med_n;
    for (i = 0; i < n; i++) a[i] = s->med[i];
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && a[j - 1] > a[j]; j--) { int x = a[j]; a[j] = a[j - 1]; a[j - 1] = x; }
    return a[n / 2];
}

static inline void bsm_init(struct bsm *s, int mv, unsigned cls, const unsigned short *t)
{
    int k;
    s->med_n = 0; s->med_i = 0;
    bsm_median(s, mv);
    for (k = 0; k < BSM_NCLS; k++) { s->meas[k] = 0; s->learned[k] = 0; }
    s->r_mohm = BSM_R_DEFAULT; s->r_n = 0;
    bsm_update_sags(s);
    s->vlearn = (int32_t)mv << 8;
    s->cls = cls;
    s->vfast = (int32_t)mv << 8;
    s->vslow = (int32_t)bsm_comp(s, s->vfast) << 8;
    s->stable_ms = 0; s->pend_bit = -1; s->pend_ms = 0; s->vpend = false;
    s->below_ms = s->above_ms = s->hold_ms = 0; s->chg_acc = 0; s->mode = 0;
    s->target_pm = bsm_pm((int)(s->vslow >> 8), t);
    s->raw_pm = bsm_pm(mv, t);
    s->display = (s->target_pm + 5) / 10;
    s->inited = true;
}

/* EMA step: x += (in - x) * dt / tau, everything x256 */
static inline int32_t bsm_ema(int32_t x, int32_t in, int32_t dt_ms, int32_t tau_ms)
{
    if (dt_ms >= tau_ms) return in;
    return x + (int32_t)(((int64_t)(in - x) * dt_ms) / tau_ms);
}

/*
 * One step.  mv = measured battery mV, dt_ms = time since the last step,
 * cls = active loads, charger = external power present, charging = the
 * charger IC is charging, chg_ma = measured charge current (< 0: unknown),
 * cap_mah = battery capacity setting, t = table.
 * Returns the percentage to show.
 */
static inline int bsm_step(struct bsm *s, int mv, int32_t dt_ms, unsigned cls,
                           bool charger, bool charging, int chg_ma, int cap_mah,
                           const unsigned short *t)
{
    int32_t v_prev = s->vlearn;
    /* 3 = external power but not charging (e.g. USB charging switched off): the battery is not used, hold */
    int mode = !charger ? 0 : charging ? 1 : mv >= 4100 ? 2 : 3;
    int target10, gap;

    if (!s->inited) { bsm_init(s, mv, cls, t); return s->display; }
    if (dt_ms <= 0) dt_ms = 1;
    if (dt_ms > 20000) s->med_n = 0;                   /* a long gap (sleep): old samples say nothing */
    mv = bsm_median(s, mv);

    /* --- learn the voltage step of a single load turning on/off (on battery only) --- */
    if (dt_ms > 2000 || mode != 0) s->pend_bit = -1;   /* gap in the samples / charging: no learning */
    if (dt_ms > 20000 || mode != 0) s->vpend = false;
    if (cls != s->cls)
    {
        unsigned diff = cls ^ s->cls;
        if (diff & (1u << BSM_VIDEO))
        {
            unsigned keep = s->cls;
            s->vpend = mode == 0;
            s->vpend_on = (cls >> BSM_VIDEO) & 1;
            s->cls &= ~(1u << BSM_VIDEO);
            s->vpend_c0 = ((int32_t)bsm_comp(s, s->vfast) << 8);      /* before: without the video's own share */
            s->cls = keep;
            s->vpend_ms = 0;
        }
        if (s->pend_bit < 0 && mode == 0 && s->stable_ms >= 6000 && (diff & (diff - 1)) == 0 && !(diff & (1u << BSM_VIDEO)))
        {
            int b = 0;
            while (!(diff & (1u << b))) b++;
            s->pend_bit = b; s->pend_on = (cls >> b) & 1; s->pend_v0 = v_prev; s->pend_ms = 0;
        }
        else s->pend_bit = -1;
        s->cls = cls;
        s->stable_ms = 0;
    }
    else s->stable_ms += dt_ms;

    s->vfast = bsm_ema(s->vfast, (int32_t)mv << 8, dt_ms, 4000);
    s->vlearn = bsm_ema(s->vlearn, (int32_t)mv << 8, dt_ms, 2000);

    if (s->vpend && (s->vpend_ms += dt_ms) >= 30000)
    {
        unsigned keep = s->cls;
        int o;
        s->cls &= ~(1u << BSM_VIDEO);
        o = (int)((s->vpend_on ? s->vpend_c0 - ((int32_t)bsm_comp(s, s->vfast) << 8)
                               : ((int32_t)bsm_comp(s, s->vfast) << 8) - s->vpend_c0) / 256);
        s->cls = keep;
        if (o >= 0 && o <= BSM_SAG_MAX)
        {
            if (s->learned[BSM_VIDEO] == 0) s->meas[BSM_VIDEO] = (int16_t)o;
            else s->meas[BSM_VIDEO] += (o - s->meas[BSM_VIDEO]) / (s->learned[BSM_VIDEO] < 3 ? 2 : 4);
            if (s->learned[BSM_VIDEO] < 1000) s->learned[BSM_VIDEO]++;
            bsm_update_sags(s);
        }
        s->vpend = false;
    }

    if (s->pend_bit >= 0)
    {
        s->pend_ms += dt_ms;
        if (s->pend_ms >= 8000)
        {
            static const int16_t ma[BSM_NCLS] = BSM_LOAD_MA;
            int32_t obs = s->pend_on ? s->pend_v0 - s->vlearn : s->vlearn - s->pend_v0;   /* x256 */
            int b = s->pend_bit, o = (int)(obs / 256);
            if (o >= -20 && o <= BSM_SAG_MAX + 20)
            {
                int32_t r;
                if (o < 0) o = 0;
                if (o > BSM_SAG_MAX) o = BSM_SAG_MAX;
                /* the load's own drop: first measurements count more, later ones average in slowly */
                if (s->learned[b] == 0) s->meas[b] = (int16_t)o;
                else s->meas[b] += (o - s->meas[b]) / (s->learned[b] < 3 ? 2 : 8);
                if (s->learned[b] < 1000) s->learned[b]++;
                /* the cell's resistance from it (bounded 50..1500 milliohm) */
                r = (int32_t)o * 1000 / ma[b];
                if (r < 50) r = 50;
                if (r > 1500) r = 1500;
                s->r_mohm += (r - s->r_mohm) / (s->r_n < 4 ? 2 : 16);
                if (s->r_n < 100000) s->r_n++;
                bsm_update_sags(s);
            }
            s->pend_bit = -1;
        }
    }

    /* --- compensated, slow estimate --- */
    s->vslow = bsm_ema(s->vslow, (int32_t)bsm_comp(s, s->vfast) << 8, dt_ms, s->balanced ? 20000 : 45000);
    s->target_pm = bsm_pm((int)(s->vslow >> 8), t);
    s->raw_pm = bsm_pm((int)(s->vfast >> 8), t);
    target10 = s->target_pm;

    /* --- mode changes --- */
    if (mode != s->mode)
    {
        if (s->mode != 0 && mode == 0) s->hold_ms = s->balanced ? 60000 : 120000;   /* unplugged: let the voltage settle */
        s->below_ms = s->above_ms = 0;
        s->mode = mode;
    }

    if (mode == 3) return s->display;
    if (mode == 2)
    {   /* charger says full: up to 100 %, 1 % per 2 s */
        s->above_ms += dt_ms;
        if (s->display < 100 && s->above_ms >= 2000) { s->display++; s->above_ms = 0; }
        return s->display;
    }

    if (mode == 1)
    {   /* charging: count up with the charge; ~400 mA from USB/charger, half of it above 80 % (CV phase) */
        int ma = 400, per_pct;
        if (cap_mah < 100) cap_mah = 100;
        if (chg_ma >= 0) ma = chg_ma > 1500 ? 1500 : chg_ma;      /* the charger's current monitor */
        else
        {   /* unknown: ~400 mA, less in the constant-voltage phase */
            if (s->display >= 80) ma /= 2;
            if (s->display >= 95) ma /= 2;
        }
        per_pct = cap_mah * 36;                     /* mA*s per 1 % = cap*3600/100 */
        s->chg_acc += ma * dt_ms / 1000;
        while (s->chg_acc >= per_pct && s->display < 99) { s->chg_acc -= per_pct; s->display++; }
        /* constant-voltage phase reached (cell at ~4.15 V while charging): the cell is past ~80 % whatever was counted */
        if ((s->vfast >> 8) >= 4150 && s->display < 80)
        {
            s->above_ms += dt_ms;
            if (s->above_ms >= 5000) { s->display++; s->above_ms = 0; }
        }
        if (s->display >= 99) s->chg_acc = 0;
        return s->display;
    }

    /* --- on battery --- */
    if (s->hold_ms > 0) { s->hold_ms -= dt_ms; return s->display; }

    gap = s->display * 10 - target10;                 /* > 0: estimate below what is shown */
    if (s->balanced)
    {   /* v92: both ways, like stock, with a dead band (0.8 % down, 1.5 % up) and a short wait; far off: 1 % per 2 s */
        if (gap >= 8)
        {
            int32_t need = gap >= 30 ? 2000 : 10000;
            s->above_ms = 0;
            s->below_ms += dt_ms;
            if (s->below_ms >= need && s->display > 0) { s->display--; s->below_ms = 0; }
        }
        else if (gap <= -15)
        {
            int32_t need = gap <= -30 ? 2000 : 30000;
            s->below_ms = 0;
            s->above_ms += dt_ms;
            if (s->above_ms >= need && s->display < 100) { s->display++; s->above_ms = 0; }
        }
        else s->below_ms = s->above_ms = 0;
    }
    else if (gap > 5)
    {
        int32_t need = gap < 30 ? 40000 : gap < 100 ? 15000 : 5000;
        s->above_ms = 0;
        s->below_ms += dt_ms;
        if (s->below_ms >= need && s->display > 0) { s->display--; s->below_ms = 0; }
    }
    else if (gap < -30)
    {   /* clearly higher for 3 minutes (a long heavy load ended, start-up under load): step up once a minute */
        s->below_ms = 0;
        s->above_ms += dt_ms;
        if (s->above_ms >= 180000 && s->display < 100) { s->display++; s->above_ms = 120000; }
    }
    else s->below_ms = s->above_ms = 0;

    /* near empty the uncompensated reading wins (never show 20 % when the cell is about to cut out) */
    if (s->raw_pm < 50 && s->display > s->raw_pm / 10 + 2) s->display = s->raw_pm / 10 + 2;
    return s->display;
}

#endif /* BATTERY_SMOOTH_H */
