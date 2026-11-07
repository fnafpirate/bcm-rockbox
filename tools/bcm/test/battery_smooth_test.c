/* Host simulation of firmware/export/battery_smooth.h against Rockbox's plain filter.
   gcc -O2 -I../../../firmware/export battery_smooth_test.c -lm && ./a.out */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "battery_smooth.h"

static const unsigned short tab[11] = { 3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180 };
static double ocv(double soc) /* 0..1 */
{
    double p = soc * 10; int i = (int)p; if (i >= 10) return tab[10]; if (i < 0) return tab[0];
    return tab[i] + (tab[i + 1] - tab[i]) * (p - i);
}
static double gauss(void) { double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0); return sqrt(-2 * log(u)) * cos(6.2831853 * v); }

struct load { int bl, boost, audio, video, disk; };
static double current_ma(struct load l) { return 25 + 30 * l.bl + 20 * l.boost + 15 * l.audio + 170 * l.video + 90 * l.disk; }

int main(int argc, char **argv)
{
    double R = argc > 1 ? atof(argv[1]) : 0.25;   /* ohm */
    double cap = 600, soc = 0.9, t;
    const double dt = 0.5;
    struct bsm s = { 0 };
    long rb_avg = 0; int rb_now = 0, rb_pct = 0, inited = 0;
    int last_disp = -1, ups = 0, rb_ups = 0, rb_last = -1, disp_moves = 0, rb_moves = 0;
    double maxerr = 0, rb_maxerr = 0, polar = 0;
    srand(1);
    for (t = 0; t < 5.0 * 3600; t += dt)
    {
        struct load l = { 0 };
        double min = t / 60, i, v;
        int mv;
        bool charger = false, charging = false;
        /* scenario */
        if (argc > 2) { soc = soc > 0.5 && t == 0 ? 0.5 : soc; l.video = 1; l.bl = 1; l.disk = fmod(t, 90) < 1.5; l.boost = fmod(t, 600) < 10; }
        else if (min < 20) { l.bl = fmod(t, 30) < 12; l.boost = fmod(t, 30) < 3; l.disk = fmod(t, 120) < 2; }
        else if (min < 70) { l.video = 1; l.audio = 1; l.bl = 1; l.boost = fmod(t, 600) < 10; l.disk = fmod(t, 90) < 1.5; }
        else if (min < 150) { l.audio = 1; l.bl = fmod(t, 300) < 10; l.boost = fmod(t, 300) < 4; l.disk = fmod(t, 600) < 8; }
        else if (min < 200) { l.bl = fmod(t, 60) < 10; charger = true; }
        else { l.audio = 1; l.bl = fmod(t, 200) < 10; }
        i = current_ma(l);
        if (charger)
        {
            double vchg = ocv(soc) + 0.4 * 1000 * R / 1000.0 * 1000 / 1000; /* placeholder */
            double ich = 400;
            (void)vchg;
            if (ocv(soc) + ich * R > 4200) ich = (4200 - ocv(soc)) / R;   /* CV phase */
            charging = ich > 40;
            if (!charging) ich = 0;
            soc += (ich - i) * dt / 3600.0 / cap;
            if (soc > 1) soc = 1;
            v = ocv(soc) + (ich - i) * R;
        }
        else
        {
            soc -= i * dt / 3600.0 / cap;
            /* polarisation: slow extra drop that follows the load (tau 60 s) */
            polar += (i * R * 0.3 - polar) * dt / 60;
            v = ocv(soc) - i * R - polar;
        }
        mv = (int)(v + gauss() * 4);
        if (soc < 0.02) break;
        /* Rockbox: no samples while the disk is active */
        if (!l.disk || charger)
        {
            unsigned cls = (l.bl << BSM_BACKLIGHT) | (l.boost << BSM_BOOST) | (l.audio << BSM_AUDIO) | (l.video << BSM_VIDEO);
            static double tlast = 0;
            int d;
            if (!inited) { rb_now = mv; rb_avg = mv * 128L; inited = 1; tlast = t; }
            rb_avg += mv - rb_avg / 128; rb_now = rb_avg / 128;
            rb_pct = bsm_pm(rb_now, tab) / 10;
            d = bsm_step(&s, mv, (int32_t)((t - tlast) * 1000 + 0.5), cls, charger, charging, -1, (int)cap, tab);
            if (t == tlast) d = bsm_step(&s, mv, 500, cls, charger, charging, -1, (int)cap, tab);
            tlast = t;
            if (!charger && t > 300)
            {
                double truep = soc * 100;
                if (last_disp >= 0 && d > last_disp) ups++;
                if (rb_last >= 0 && rb_pct > rb_last) rb_ups++;
                if (fabs(d - truep) > maxerr) maxerr = fabs(d - truep);
                if (fabs(rb_pct - truep) > rb_maxerr) rb_maxerr = fabs(rb_pct - truep);
            }
            if (last_disp >= 0 && d != last_disp) disp_moves++;
            if (rb_last >= 0 && rb_pct != rb_last) rb_moves++;
            last_disp = d; rb_last = rb_pct;
        }
        if (fmod(t, argc > 2 ? 300 : 600) < dt)
            printf("%5.0f min  true %5.1f %%  rockbox %3d %%  smooth %3d %%  (est %4.1f raw %4.1f)  sag bl %d boost %d audio %d video %d %s\n",
                   min, soc * 100, rb_pct, s.display, s.target_pm / 10.0, s.raw_pm / 10.0, s.sag[0], s.sag[1], s.sag[2], s.sag[3],
                   charger ? (charging ? "CHARGING" : "FULL") : "");
    }
    printf("R=%.2f  on battery: smooth went UP %d times, max error %.1f %%, changes %d | rockbox went UP %d times, max error %.1f %%, changes %d\n",
           R, ups, maxerr, disp_moves, rb_ups, rb_maxerr, rb_moves);
    return 0;
}
