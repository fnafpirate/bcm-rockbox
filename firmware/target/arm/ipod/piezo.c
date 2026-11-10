/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006-2007 Robert Keevil
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

#include "thread.h"
#include "system.h"
#include "kernel.h"
#include "usb.h"
#include "logf.h"
#include "piezo.h"
#include "pcm_sink.h"
#ifdef HAVE_PIEZO_SPEAKER
#define HAVE_PIEZO_SPEAKER_CODE
#endif
#ifdef HAVE_PIEZO_SPEAKER_CODE
#include "timer.h"
#include "button.h"
#include "sound.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_sampr.h"
#include "pcm_sink.h"
#include "pcm_mixer.h"
#endif

static long piezo_stack[DEFAULT_STACK_SIZE/sizeof(long)];
static const char piezo_thread_name[] = "piezo";
static struct event_queue piezo_queue;
static unsigned int duration;
static bool beeping;

enum {
    Q_PIEZO_BEEP = 1,
    Q_PIEZO_BEEP_FOR_TICK,
    Q_PIEZO_BEEP_FOR_USEC,
    Q_PIEZO_STOP,
    Q_PIEZO_TUNE,       /* bcm overlay v77 */
    Q_PIEZO_SAMPLES
};

#ifdef HAVE_PIEZO_SPEAKER_CODE
/***************************************************************************
 * bcm overlay v77: more than a click.
 *
 * The click element sits on PWM0 (0x7000a000): bit 31 on, bits 16-23 the "form" -- the duty cycle in 1/256 (the
 * click uses 0x80, a square wave) -- and bits 0-15 the period (10 = 8.5 kHz, see piezo_hz below).
 *  - tunes: a list of notes played by the piezo thread (start-up / shutdown chimes)
 *  - samples: 8-bit sound played from the user timer every 90 us (11111 Hz): each sample sets the duty cycle of a
 *    fast carrier ("PWM DAC": the element and the ear average the carrier away) or, for "resonant", the duty of a
 *    carrier near the element's own resonance (louder, but a whine under everything)
 *  - a PCM sink (firmware/pcm.c): Settings > Sound Settings > Click Speaker routes music, videos and the USB DAC to
 *    the piezo when no headphones are in (or always).  Mono, ~11 kHz, about 7 bits, quiet: a "crappy little
 *    speaker" by design.  Nothing here could be tried on hardware when it was written: the carrier period, the
 *    method and the boost are settings so the best ones can be found on the iPod itself.
 ***************************************************************************/
/* v78: two tick rates (Settings: Quality): Normal 90 us = 11111 Hz, High 45 us = 22222 Hz (treble up to ~10 kHz, twice
   the interrupts: ~5 % of the CPU at 80 MHz) */
static unsigned pz_tick_us = 90;
#define PZ_RATE     (1000000 / pz_tick_us)
int piezo_method;                 /* 0 PWM DAC, 1 resonant carrier */
int piezo_carrier;                /* the carrier period, 0 = automatic (4 for the DAC, 26 for resonant) */
int piezo_boost = 2;              /* x1, x2, x4, x8 */
int piezo_speaker_mode;           /* 0 off, 1 when no headphones are in, 2 always */
int piezo_loudness = 1;           /* v78: automatic level (a compressor): quiet passages brought up */
volatile bool piezo_docked;       /* set by the dock code (apps/iap/iap-lingo0.c): the sound goes to the dock */

static unsigned pz_last;
static unsigned pz_per;
static int pz_quiet;              /* ticks of silence: the carrier is switched off after 50 ms of it */
static int pz_quiet_max = 555;    /* 50 ms in ticks */
static inline void pz_out(unsigned level)
{
    unsigned v;
    int d = (int)level - 128;
    if (d > -2 && d < 2) { if (pz_quiet < pz_quiet_max) pz_quiet++; }
    else pz_quiet = 0;
    if (pz_quiet >= pz_quiet_max)
        v = 0;
    else if (piezo_method == 1)
    {   /* resonant: the carrier's strength follows the sound's size (|d|, 0..127 -> duty 0..50 %) */
        unsigned a = d < 0 ? -d : d;
        v = 0x80000000 | (a << 16) | pz_per;
    }
    else
        v = 0x80000000 | (level << 16) | pz_per;
    if (v != pz_last)
    {
        PWM0_CTRL = v;
        pz_last = v;
    }
}
static void pz_set_carrier(void)
{
    pz_per = piezo_carrier > 0 ? (unsigned)piezo_carrier : piezo_method == 1 ? 26 : 4;
}

/* ---- v78: shaping the sound for the element.  A piezo disc has almost no bass and a strong resonance (a few kHz);
   bass only eats the 8-bit range and makes it clip.  Per sample (16-bit in, an 8-bit duty out):
     1. bass cut: first-order high-pass at ~350 Hz
     2. loudness (optional): a compressor -- the level follows the sound's envelope (fast attack, ~0.3 s release),
        up to x8 for quiet parts, so speech and quiet music stay audible on a quiet speaker
     3. volume (the volume setting), Boost
     4. soft clip: peaks above 3/4 of full scale are squeezed instead of cut flat
     5. 8 bits with error feedback (the rounding error goes into the next sample: less hiss in the audible band) */
static int32_t dsp_x1, dsp_y1, dsp_env = 256, dsp_err;
static int dsp_hp_a = 26900;      /* q15 */
static int dsp_agc = 256;         /* q8 */
static unsigned dsp_n;
static void dsp_reset(void)
{
    dsp_x1 = dsp_y1 = dsp_err = 0; dsp_env = 256; dsp_agc = 256; dsp_n = 0;
    dsp_hp_a = pz_tick_us <= 50 ? 29700 : 26900;  /* exp(-2 pi 350 / fs) */
    pz_quiet_max = (int)PZ_RATE / 20;
}
static inline unsigned dsp_run(int32_t x, int gain_q12, int boost)
{
    int32_t y, a, v, q;
    y = (dsp_hp_a * (dsp_y1 + x - dsp_x1)) >> 15;
    dsp_x1 = x; dsp_y1 = y;
    if (y > 32767) y = 32767; else if (y < -32767) y = -32767;
    if (piezo_loudness)
    {
        a = y < 0 ? -y : y;
        if (a > dsp_env) dsp_env += (a - dsp_env) >> 2;                  /* attack ~0.4 ms */
        else dsp_env -= dsp_env >> (pz_tick_us <= 50 ? 12 : 11);       /* release ~0.2 s */
        if (dsp_env < 64) dsp_env = 64;
        if (!(++dsp_n & 31))
        {   /* aim the envelope at ~1/3 of full scale; between x1 and x8 */
            int g = dsp_env < 160 ? 256 : (int)((10000L << 8) / dsp_env);   /* near silence: no gain (no hiss) */
            dsp_agc = g < 256 ? 256 : g > 2048 ? 2048 : g;
        }
        y = (y * dsp_agc) >> 8;
    }
    y = ((y * gain_q12) >> 12) << boost;          /* |y| <= 262136 * 4096: fits */
    a = y < 0 ? -y : y;
    if (a > 24576)
    {
        a = 24576 + ((a - 24576) >> 2);
        if (a > 32767) a = 32767;
        y = y < 0 ? -a : a;
    }
    v = y + dsp_err;
    q = v >> 8;
    dsp_err = v - (q << 8);
    q += 128;
    return q < 0 ? 0 : q > 255 ? 255 : (unsigned)q;
}

/* ---- one-shot samples (start-up voice, the test sounds): 8-bit at their own rate, resampled to the tick */
static const unsigned char *volatile smp;
static volatile unsigned smp_n;
static volatile bool smp_on;
static unsigned smp_rate = 16000;
static uint32_t smp_ph, smp_step;

/* ---- the PCM sink */
extern struct pcm_sink piezo_pcm_sink;
static const int16_t *volatile sk_buf;
static volatile size_t sk_frames;
static size_t sk_pos;
static uint32_t sk_phase, sk_step;
static unsigned long sk_rate = 44100;
static volatile int sk_lock;
static volatile bool sk_run;
static int sk_gain;               /* q12 */
static int sk_vol_last = 1000;
static uint16_t sk_db_tab[61];    /* q12: 0 .. -60 dB */

static void sk_update_gain(void)
{
    int v = sound_current(SOUND_VOLUME), nd = sound_numdecimals(SOUND_VOLUME), db;
    if (v == sk_vol_last) return;
    sk_vol_last = v;
    db = nd ? v / 10 : v;                 /* dB */
    if (db > 0) db = 0;
    sk_gain = db < -60 ? 0 : sk_db_tab[-db];
}
static void sk_calc_step(void)
{
    sk_step = (uint32_t)(((uint64_t)sk_rate << 16) / PZ_RATE);
    smp_step = (uint32_t)(((uint64_t)smp_rate << 16) / PZ_RATE);
}

static volatile bool pz_timer_on;
static void pz_isr(void)
{
    if (smp_on)
    {
        uint32_t i = smp_ph >> 16;
        if (i + 1 < smp_n)
        {
            uint32_t f = smp_ph & 0xffff;
            int a = smp[i], b2 = smp[i + 1];
            pz_out((unsigned)(a + (int)(((b2 - a) * (int32_t)f) >> 16)));
            smp_ph += smp_step;
        }
        else
        {
            smp_on = false;
            pz_last = 0;
            PWM0_CTRL = 0;
        }
        return;
    }
    if (!sk_run || sk_lock)
        return;
    {
        uint32_t adv;
        const int16_t *b;
        int32_t sum = 0, n = 0, s;
        sk_phase += sk_step;
        adv = sk_phase >> 16;
        sk_phase &= 0xffff;
        b = sk_buf;
        /* average the frames this tick covers (a crude low-pass against aliasing) */
        while (adv--)
        {
            if (sk_pos >= sk_frames)
            {
                const void *p;
                size_t sz;
                if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &p, &sz) || sz < 4)
                {
                    sk_run = false;
                    pz_last = 0;
                    PWM0_CTRL = 0;
                    return;
                }
                sk_buf = b = p;
                sk_frames = sz / 4;
                sk_pos = 0;
                pcm_play_dma_status_callback(PCM_DMAST_STARTED);
                sk_update_gain();
            }
            sum += b[2 * sk_pos] + b[2 * sk_pos + 1];
            n += 2;
            sk_pos++;
        }
        if (!n)
            return;
        s = sum / n;                                      /* -32768..32767 */
        pz_out(dsp_run(s, sk_gain, piezo_boost));
    }
}
static void pz_timer_gone(void)                   /* a plugin took the user timer */
{
    pz_timer_on = false;
    sk_run = false;
    smp_on = false;
}
static bool pz_timer_start(void)
{
    if (!pz_timer_on)
    {
        pz_set_carrier();
        sk_calc_step();
        dsp_reset();
        pz_last = 0;
        pz_quiet = 0;
        pz_timer_on = timer_register(0, pz_timer_gone, pz_tick_us * (TIMER_FREQ / 1000000), pz_isr IF_COP(, CPU));
    }
    return pz_timer_on;
}
static void pz_timer_check(void)                 /* piezo thread: let the timer go when nothing uses it */
{
    if (pz_timer_on && !smp_on && !sk_run)
    {
        timer_unregister();
        pz_timer_on = false;
        PWM0_CTRL = 0;
        pz_last = 0;
    }
}
bool piezo_speaker_active(void)
{
    return smp_on || sk_run;
}

/* sink ops */
static void sk_nop(void) { }
static void sk_init(void)
{
    int i;
    uint32_t g = 4096;
    for (i = 0; i <= 60; i++)
    {
        sk_db_tab[i] = g;
        g = (g * 7298 + 4096) >> 13;            /* -1 dB */
    }
}
static void sk_set_freq(uint16_t freq)
{
    sk_rate = hw_freq_sampr[freq];
    sk_calc_step();
}
static void sk_lock_op(void) { sk_lock++; }
static void sk_unlock_op(void) { sk_lock--; }
static void sk_play(const void *addr, size_t size)
{
    sk_lock++;
    sk_buf = addr;
    sk_frames = size / 4;
    sk_pos = 0;
    sk_phase = 0;
    sk_vol_last = 1000;
    sk_update_gain();
    if (!sk_step) sk_set_freq(piezo_pcm_sink.pending_freq);
    if (!pz_timer_on) dsp_reset();
    sk_run = true;
    sk_lock--;
    if (!pz_timer_start())
        sk_run = false;
}
static void sk_stop(void)
{
    sk_run = false;
    if (!smp_on)
    {
        pz_last = 0;
        PWM0_CTRL = 0;
    }
}
struct pcm_sink piezo_pcm_sink = {
    .caps = {
        .samprs       = hw_freq_sampr,
        .num_samprs   = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
        .volume_type  = PCM_SINK_HWVOL,
    },
    .ops = {
        .init     = sk_init,
        .postinit = sk_nop,
        .set_freq = sk_set_freq,
        .lock     = sk_lock_op,
        .unlock   = sk_unlock_op,
        .play     = sk_play,
        .stop     = sk_stop,
    },
};

/* where the sound goes when nothing else (USB iAP) has it */
static bool sk_wanted(void)
{
    return piezo_speaker_mode == 2 ||
           (piezo_speaker_mode == 1 && !headphones_inserted() && !piezo_docked);
}
int piezo_speaker_home_sink(void)
{
    return sk_wanted() ? PCM_SINK_PIEZO : PCM_SINK_BUILTIN;
}
static void sk_route(void)
{
    enum pcm_sink_ids cur = pcm_current_sink();
    bool want = sk_wanted();
    if (want && cur == PCM_SINK_BUILTIN)
        mixer_switch_sink(PCM_SINK_PIEZO);
    else if (!want && cur == PCM_SINK_PIEZO)
        mixer_switch_sink(PCM_SINK_BUILTIN);
}
#else
bool piezo_speaker_active(void) { return false; }
int piezo_speaker_home_sink(void) { return 0; }
#endif /* HAVE_PIEZO_SPEAKER_CODE */

static inline void piezo_hw_init(void)
{
#ifndef SIMULATOR
    /*logf("PIEZO: hw_init");*/
    DEV_INIT1 &= ~0xc;
    DEV_EN |= DEV_PWM;
#endif
}

static void piezo_hw_tick(unsigned int form_and_period)
{
#ifndef SIMULATOR
    PWM0_CTRL = 0x80000000 | form_and_period;
#endif
}

static inline void piezo_hw_stop(void)
{
#ifndef SIMULATOR
    PWM0_CTRL = 0;
#endif
}

static void piezo_thread(void)
{
    struct queue_event ev;
    long piezo_usec_off;

    while(1)
    {
#ifdef HAVE_PIEZO_SPEAKER_CODE
        queue_wait_w_tmo(&piezo_queue, &ev, HZ / 4);
        /* the speaker / a sample owns the element: no clicks or beeps over it */
        if (piezo_speaker_active() && ev.id >= Q_PIEZO_BEEP && ev.id <= Q_PIEZO_STOP)
            continue;
#else
        queue_wait(&piezo_queue, &ev);
#endif
        switch(ev.id)
        {
            case Q_PIEZO_BEEP:
                piezo_hw_tick((unsigned int)ev.data);
                beeping = true;
                break;
            case Q_PIEZO_BEEP_FOR_TICK:
                piezo_hw_tick((unsigned int)ev.data);
                beeping = true;
                sleep(duration);
                if (beeping)
                    piezo_hw_stop();
                beeping = false;
                /* remove anything that appeared while sleeping */
                queue_clear(&piezo_queue);
                break;
            case Q_PIEZO_BEEP_FOR_USEC:
                piezo_usec_off = USEC_TIMER + duration;
                piezo_hw_tick((unsigned int)ev.data);
                beeping = true;
                while (TIME_BEFORE(USEC_TIMER, piezo_usec_off))
                    if (duration >= 5000) yield();
                if (beeping)
                    piezo_hw_stop();
                beeping = false;
                /* remove anything that appeared while sleeping */
                queue_clear(&piezo_queue);
                break;
            case Q_PIEZO_STOP:
                if (beeping)
                    piezo_hw_stop();
                beeping = false;
                break;
#ifndef SIMULATOR
            case SYS_USB_CONNECTED:
                /*logf("USB: Piezo core");*/
                piezo_hw_stop();
                queue_clear(&piezo_queue);
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                usb_wait_for_disconnect(&piezo_queue);
                break ;
#endif
#ifdef HAVE_PIEZO_SPEAKER_CODE
            case Q_PIEZO_TUNE:
            {   /* {hz, ms} pairs, hz 0 = a rest, ends with {0, 0} */
                const unsigned short *t = (const unsigned short *)ev.data;
                for (; t[0] || t[1]; t += 2)
                {
                    if (t[0]) piezo_hw_tick(0x80 << 16 | piezo_hz(t[0]));
                    else piezo_hw_stop();
                    sleep(MAX(1, t[1] * HZ / 1000));
                }
                piezo_hw_stop();
                break;
            }
            case Q_PIEZO_SAMPLES:
                /* played from the timer; the next message waits until it is done (chime, then the voice) */
                if (pz_timer_start() || smp_on)
                {
                    smp_on = false;
                    smp_ph = 0;
                    smp_on = true;
                    while (smp_on)
                        sleep(HZ / 20);
                }
                pz_timer_check();
                break;
#endif
            case SYS_TIMEOUT:
#ifdef HAVE_PIEZO_SPEAKER_CODE
                sk_route();
                pz_timer_check();
#endif
                break;
        }
        yield();
    }
}


void piezo_play(unsigned short inv_freq, unsigned char form)
{
    queue_post(&piezo_queue, Q_PIEZO_BEEP,
        (intptr_t)((unsigned int)form << 16 | inv_freq));
}

void piezo_play_for_tick(unsigned short inv_freq,
                    unsigned char form, unsigned int dur)
{
    duration = dur;
    queue_post(&piezo_queue, Q_PIEZO_BEEP_FOR_TICK,
        (intptr_t)((unsigned int)form << 16 | inv_freq));
}

void piezo_play_for_usec(unsigned short inv_freq,
                    unsigned char form, unsigned int dur)
{
    duration = dur;
    queue_post(&piezo_queue, Q_PIEZO_BEEP_FOR_USEC,
        (intptr_t)((unsigned int)form << 16 | inv_freq));
}

#ifdef HAVE_PIEZO_SPEAKER_CODE
void piezo_play_tune(const unsigned short *notes)
{
    queue_post(&piezo_queue, Q_PIEZO_TUNE, (intptr_t)notes);
}
/* 8-bit unsigned samples at `rate` Hz (resampled to the tick); the data must stay until piezo_busy() is false */
void piezo_play_samples(const unsigned char *u8, unsigned n, unsigned rate)
{
    while (smp_on) sleep(1);
    smp = u8;
    smp_n = n;
    smp_rate = rate;
    sk_calc_step();
    queue_post(&piezo_queue, Q_PIEZO_SAMPLES, 0);
}
void piezo_settings(int method, int carrier, int boost, int speaker, int quality, int loudness)
{
    unsigned us = quality ? 45 : 90;
    piezo_method = method;
    piezo_carrier = carrier;
    piezo_boost = boost;
    piezo_speaker_mode = speaker;
    piezo_loudness = loudness;
    pz_set_carrier();
    if (us != pz_tick_us)
    {
        pz_tick_us = us;
        sk_calc_step();
        dsp_reset();
        if (pz_timer_on)
            timer_set_period(pz_tick_us * (TIMER_FREQ / 1000000));
    }
}
#endif

void piezo_stop(void)
{
    queue_post(&piezo_queue, Q_PIEZO_STOP, 0);
}

void piezo_clear(void)
{
    queue_clear(&piezo_queue);
    piezo_stop();
}

bool piezo_busy(void)
{
#ifdef HAVE_PIEZO_SPEAKER_CODE
    return !queue_empty(&piezo_queue) || smp_on;
#else
    return !queue_empty(&piezo_queue);
#endif
}

/*  conversion factor based on the following data

        period          Hz
        10              8547
        20              4465
        30              3024
        40              2286
        50              1846
        60              1537
        70              1320
        80              1165
        90              1030
        100             928

    someone with better recording/analysing equipment should be able
    to get more accurate figures
*/
unsigned int piezo_hz(unsigned int hz)
{
    if (hz > 0)
        return 91225/hz;
    else
        return 0;
}

void piezo_init(void)
{
    /*logf("PIEZO: init");*/
    piezo_hw_init();
    queue_init(&piezo_queue, true);
    create_thread(piezo_thread, piezo_stack, sizeof(piezo_stack), 0,
            piezo_thread_name IF_PRIO(, PRIORITY_REALTIME)
            IF_COP(, CPU));
}

void piezo_button_beep(bool beep, bool force)
{
    /* old on clickwheel action - piezo_play_for_usec(50, 0x80, 400);
       old on button action - piezo_play_for_usec(50, 0x80, 3000); */

#ifdef HAVE_PIEZO_SPEAKER_CODE
    if (piezo_speaker_active())
        return;                         /* v77: music / a sample on the piezo: no clicks over it */
#endif
    if (force)
        piezo_clear();

    if (queue_empty(&piezo_queue))
    {
        if (beep)
            piezo_play_for_tick(40, 0x80, HZ/5);
        else
            piezo_play_for_usec(91, 0x80, 4000);
    }
}
