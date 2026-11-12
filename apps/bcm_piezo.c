/***************************************************************************
 * bcm overlay v77: the click speaker (piezo) -- start-up / shutdown sounds, "Rockbox Plus", and its settings.
 *
 * Settings > Sound Settings > Click Speaker:
 *   Speaker for Music & Video   Off / When No Headphones / Always   (firmware/target/arm/ipod/piezo.c: a PCM sink)
 *   Method                      PWM DAC (cleaner) / Resonant (louder, whines)
 *   Carrier                     Auto, or a PWM period of 2..60 (try a few: the best one depends on the element)
 *   Boost                       x1 / x2 / x4 / x8 (over the volume setting; loud parts clip)
 *   Start-up Sound              Off / Chime / "Rockbox Plus" / Chime + "Rockbox Plus"
 *   Shutdown Sound              Off / On
 *   Test Start-up Sound
 * The voice is apps/bcm_piezo_voice.h (tools/bcm/piezo_voice.py), or /.rockbox/piezo/boot.wav when that exists:
 * any PCM WAV (8 / 16-bit, mono / stereo, any rate), its first 5 s.
 * v88: your own sounds -- any one file (any name; MP3, M4A, FLAC, OGG, WAV ...) in
 *   /.rockbox/startup chime     replaces the start-up chime and voice
 *   /.rockbox/charging chime    replaces the charging-complete chime
 * Empty folder = the built-in sound.  Both folders are made at start-up when missing.  A new file is converted once
 * (plugin viewers/piezo_snd.rock, "Preparing click speaker sound...") and kept in /.rockbox/piezo.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later
 * version.
 ****************************************************************************/
#include "config.h"
#include <stdbool.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "settings.h"
#include "core_alloc.h"
#include "bcm_piezo.h"

#if defined(IPOD_VIDEO) && !defined(SIMULATOR)
#include "piezo.h"
#include "bcm_piezo_voice.h"
#include "dir.h"
#include "rbpaths.h"
#include "plugin.h"
#include "splash.h"
#include "lang.h"

#define BOOT_WAV ROCKBOX_DIR "/piezo/boot.wav"
#define MAX_SECONDS 5                     /* v88: 3 -> 5 s (the start-up / charging sounds) */
#define SAMPLE_RATE 16000                 /* v78: the voice / WAV / test sounds are kept at 16 kHz */

static const unsigned short chime_up[] = { 1047, 70, 1319, 70, 1568, 70, 2093, 150, 0, 40, 0, 0 };
static const unsigned short chime_down[] = { 2093, 70, 1568, 70, 1319, 70, 1047, 160, 0, 0 };

static unsigned char wav_buf[MAX_SECONDS * SAMPLE_RATE];
static unsigned char chg_buf[MAX_SECONDS * SAMPLE_RATE];   /* v88: the charging chime stays in memory (USB mode) */
static unsigned chg_n;
#define DIR_STARTUP  ROCKBOX_DIR "/startup chime"
#define DIR_CHARGING ROCKBOX_DIR "/charging chime"
#define CACHE_DIR    ROCKBOX_DIR "/piezo"
#define DECODER      VIEWERS_DIR "/piezo_snd.rock"

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned long rd32(const unsigned char *p) { return rd16(p) | (unsigned long)rd16(p + 2) << 16; }

/* the WAV, as 8-bit unsigned at the piezo's rate, loudest part at full scale: the number of samples, 0 = none */
static unsigned load_wav(const char *path, unsigned char *dst)
{
    unsigned char h[44], chunk[512];
    int fd = open(path, O_RDONLY);
    unsigned ch = 0, bits = 0, out = 0, rate_out = SAMPLE_RATE, n, i;
    unsigned long rate = 0, left = 0, pos = 0, step;
    int peak = 1, prev = 0;
    if (fd < 0)
        return 0;
    if (read(fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        goto fail;
    for (;;)                                  /* chunks: "fmt " then "data" */
    {
        unsigned long len;
        if (read(fd, h, 8) != 8)
            goto fail;
        len = rd32(h + 4);
        if (!memcmp(h, "fmt ", 4))
        {
            if (len < 16 || len > sizeof chunk || read(fd, chunk, len) != (ssize_t)len)
                goto fail;
            if (rd16(chunk) != 1)
                goto fail;                    /* PCM only */
            ch = rd16(chunk + 2);
            rate = rd32(chunk + 4);
            bits = rd16(chunk + 14);
            if (len & 1) lseek(fd, 1, SEEK_CUR);
        }
        else if (!memcmp(h, "data", 4))
        {
            left = len;
            break;
        }
        else
            lseek(fd, (len + 1) & ~1UL, SEEK_CUR);
    }
    if (!ch || ch > 2 || (bits != 8 && bits != 16) || rate < 4000 || rate > 192000)
        goto fail;
    /* resample: output k sits at input position k * step (16.16), between frames floor(pos) and floor(pos) + 1 */
    step = (unsigned long)(((unsigned long long)rate << 16) / rate_out);
    {
        unsigned fsz = ch * bits / 8;
        unsigned long frame = 0;
        short *tmp;                           /* 16-bit first, then scaled to 8 bits by the loudest sample */
        int hnd = core_alloc_ex(sizeof wav_buf * sizeof(short), NULL);
        if (hnd <= 0)
            goto fail;
        core_pin(hnd);
        tmp = core_get_data(hnd);
        while (left >= fsz && out < sizeof wav_buf)
        {
            n = MIN(sizeof chunk / fsz * fsz, left);
            if (read(fd, chunk, n) != (ssize_t)n)
                break;
            left -= n;
            for (i = 0; i + fsz <= n && out < sizeof wav_buf; i += fsz, frame++)
            {
                int v;
                if (bits == 8)
                    v = ch == 2 ? ((int)chunk[i] + chunk[i + 1] - 256) << 7 : ((int)chunk[i] - 128) << 8;
                else
                    v = ch == 2 ? ((short)rd16(chunk + i) + (short)rd16(chunk + i + 2)) / 2 : (short)rd16(chunk + i);
                if (frame)
                    while ((pos >> 16) == frame - 1 && out < sizeof wav_buf)
                    {
                        int f = (int)(pos & 0xffff), o = prev + (int)(((long long)(v - prev) * f) >> 16);
                        tmp[out++] = (short)o;
                        if (o < 0) o = -o;
                        if (o > peak) peak = o;
                        pos += step;
                    }
                prev = v;
            }
        }
        {   /* v78: the element has no bass: cut below ~300 Hz before scaling, so the loudest part is what it can play */
            long x1 = 0, y1 = 0;
            peak = 1;
            for (i = 0; i < out; i++)
            {
                long y = (28100L * (y1 + tmp[i] - x1)) >> 15;     /* exp(-2 pi 300 / 16000) */
                x1 = tmp[i]; y1 = y;
                if (y > 32767) y = 32767; else if (y < -32767) y = -32767;
                tmp[i] = (short)y;
                if ((y < 0 ? -y : y) > peak) peak = (int)(y < 0 ? -y : y);
            }
        }
        for (i = 0; i < out; i++)
        {
            int v = 128 + tmp[i] * 127 / peak;
            dst[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        core_unpin(hnd);
        core_free(hnd);
    }
    close(fd);
    return out;
fail:
    close(fd);
    return 0;
}

/* v88: the user's sound in `dir` (the first file by name; hidden files and Windows / macOS clutter skipped) -> dst as
   8-bit 16 kHz, the number of samples (0 = none: the built-in sound).  The decoded copy is kept as `cache` with a stamp
   of the source (name|size|date) in cache".id"; a file that could not be decoded is remembered there as well, so it is
   not tried again at every start-up. */
static unsigned prepare_sound(const char *dir, const char *cache, unsigned char *dst)
{
    char best[MAX_PATH], src[MAX_PATH], idpath[MAX_PATH], stamp[MAX_PATH + 40], old[MAX_PATH + 40];
    DIR *d;
    struct dirent *e;
    struct dirinfo bi = { 0, 0, 0 };
    unsigned n;
    int fd, len;
    best[0] = 0;
    if (!dir_exists(dir)) { mkdir(dir); return 0; }
    d = opendir(dir);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL)
    {
        struct dirinfo inf = dir_get_info(d, e);
        if (e->d_name[0] == '.' || (inf.attribute & ATTR_DIRECTORY) || inf.size <= 0
            || !strcasecmp(e->d_name, "desktop.ini") || !strcasecmp(e->d_name, "thumbs.db"))
            continue;
        if (!best[0] || strcasecmp(e->d_name, best) < 0)
        {
            strlcpy(best, e->d_name, sizeof best);
            bi = inf;
        }
    }
    closedir(d);
    if (!best[0]) return 0;
    snprintf(src, sizeof src, "%s/%s", dir, best);
    {   const char *ext = strrchr(best, '.');
        if (ext && !strcasecmp(ext, ".wav") && (n = load_wav(src, dst)) != 0)
            return n;                                  /* a PCM WAV: read as it is */
    }
    snprintf(stamp, sizeof stamp, "%s|%ld|%ld", best, (long)bi.size, (long)bi.mtime);
    snprintf(idpath, sizeof idpath, "%s.id", cache);
    old[0] = 0;
    fd = open(idpath, O_RDONLY);
    if (fd >= 0) { len = read(fd, old, sizeof old - 1); old[len > 0 ? len : 0] = 0; close(fd); }
    if (!strcmp(old, stamp) && (n = load_wav(cache, dst)) != 0)
        return n;                                      /* decoded before */
    if (!strncmp(old, "FAIL|", 5) && !strcmp(old + 5, stamp))
        return 0;                                      /* this file could not be decoded: the built-in sound */
    if (!file_exists(DECODER)) return 0;
    if (!dir_exists(CACHE_DIR)) mkdir(CACHE_DIR);
    remove(cache);
    {   char param[2 * MAX_PATH + 8];
        snprintf(param, sizeof param, "%s\n%s\n%d", src, cache, MAX_SECONDS);
        splash(0, ID2P(LANG_PIEZO_PREPARING));
        plugin_load(DECODER, param);
    }
    n = file_exists(cache) ? load_wav(cache, dst) : 0;
    fd = open(idpath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        if (!n) write(fd, "FAIL|", 5);
        write(fd, stamp, strlen(stamp));
        close(fd);
    }
    return n;
}

static void charge_setup(void)
{
    chg_n = prepare_sound(DIR_CHARGING, CACHE_DIR "/charging.wav", chg_buf);
    piezo_charge_alert(global_settings.bcm_piezo_charged, chg_buf, chg_n, SAMPLE_RATE);
}

void bcm_piezo_apply(void)
{
    /* v80 Sound Mode: Custom = the items below; the two presets override them (the items keep their values):
         Maximum Loudness: (v86) PWM DAC, bass cut at 500 Hz, +10 dB around 3.2 kHz (where the element is loudest),
                           x16 automatic gain, the volume setting ignored, 22 kHz
         Highest Quality:  PWM DAC (cleanest), 22 kHz, Boost x2, Automatic Loudness, second-order noise shaping */
    switch (global_settings.bcm_piezo_max)
    {
    case 1:   /* v86: the PWM DAC (real sound) instead of v80's resonant buzz: bass cut at 500 Hz, +10 dB at 3.2 kHz,
                 x16 automatic gain, x8, 22 kHz */
        piezo_settings(0, 0, 3, global_settings.bcm_piezo_speaker, 1, 1);
        break;
    case 2:
        piezo_settings(0, 0, 1, global_settings.bcm_piezo_speaker, 1, 1);
        break;
    default:
        piezo_settings(global_settings.bcm_piezo_method, global_settings.bcm_piezo_carrier,
                       global_settings.bcm_piezo_boost, global_settings.bcm_piezo_speaker,
                       global_settings.bcm_piezo_quality, global_settings.bcm_piezo_loudness);
        break;
    }
    piezo_set_clarity(global_settings.bcm_piezo_clarity);
    piezo_set_max(global_settings.bcm_piezo_max);
    piezo_charge_alert(global_settings.bcm_piezo_charged, chg_buf, chg_n, SAMPLE_RATE);   /* v88 */
}

static void play_boot(int what, unsigned custom)
{
    if (custom)
    {   /* v88: the user's sound replaces the chime and the voice */
        piezo_play_samples(wav_buf, custom, SAMPLE_RATE);
        return;
    }
    if (what & 1)
        piezo_play_tune(chime_up);
    if (what & 2)
    {
        unsigned n = load_wav(BOOT_WAV, wav_buf);    /* the v77 place, still read */
        if (n)
            piezo_play_samples(wav_buf, n, SAMPLE_RATE);
        else
            piezo_play_samples(bcm_piezo_voice, sizeof bcm_piezo_voice, BCM_PIEZO_VOICE_RATE);
    }
}

void bcm_piezo_startup(void)
{
    unsigned custom;
    bcm_piezo_apply();
    custom = prepare_sound(DIR_STARTUP, CACHE_DIR "/startup.wav", wav_buf);
    charge_setup();                                  /* v88: decoded (if needed) before anything plays */
    if (global_settings.bcm_piezo_boot)
        play_boot(global_settings.bcm_piezo_boot, custom);
}

int bcm_piezo_test(void)
{
    unsigned custom;
    while (piezo_busy()) sleep(HZ / 20);             /* wav_buf may still be playing */
    bcm_piezo_apply();
    custom = prepare_sound(DIR_STARTUP, CACHE_DIR "/startup.wav", wav_buf);
    play_boot(global_settings.bcm_piezo_boot ? global_settings.bcm_piezo_boot : 3, custom);
    return 0;
}

/* v88 */
int bcm_piezo_test_charged(void)
{
    while (piezo_busy()) sleep(HZ / 20);
    bcm_piezo_apply();
    charge_setup();
    piezo_play_charged();
    return 0;
}

/* v78: Test Music: 4 s made here -- a little tune with chords and a bass line, plucked notes (harmonics, decay): what
   music sounds like through the current settings.  Same path as the start-up voice (samples, not the tone generator). */
static int isin(unsigned ph)                       /* ph 0..65535 = one cycle -> -32767..32767 (parabolic) */
{
    int x = (int)(ph & 0xffff) - 32768;            /* -32768..32767 = -pi..pi */
    int y = (int)((4L * x * (32768 - (x < 0 ? -x : x))) >> 15);
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}
static long music_at(unsigned i, unsigned note)
{
    static const unsigned short mel[16] = { 523, 659, 784, 1047, 988, 784, 659, 587, 523, 659, 784, 880, 784, 659, 587, 523 };
    static const unsigned short bass[4] = { 131, 175, 196, 131 };
    unsigned k = i / note, t = i % note, bt = i % (note * 4);
    long env = 32767L - (long)t * 32767 / note, benv = 32767L - (long)bt * 24000 / (note * 4);
    unsigned long ph = (unsigned long)i * mel[k & 15] * 65536UL / SAMPLE_RATE;
    unsigned long bp = (unsigned long)i * bass[(i / (note * 4)) & 3] * 65536UL / SAMPLE_RATE;
    return (isin((unsigned)ph) + isin((unsigned)(2 * ph)) / 2 + isin((unsigned)(3 * ph)) / 4) * env / 65536
           + (isin((unsigned)bp) + isin((unsigned)(2 * bp)) / 2) * benv / 98304;
}
int bcm_piezo_test_music(void)
{
    const unsigned n = MAX_SECONDS * SAMPLE_RATE, note = n / 16;
    unsigned i;
    long peak = 1;
    while (piezo_busy()) sleep(HZ / 20);           /* wav_buf may still be playing */
    for (i = 0; i < n; i++) { long v = music_at(i, note); if ((v < 0 ? -v : v) > peak) peak = v < 0 ? -v : v; }
    for (i = 0; i < n; i++) wav_buf[i] = (unsigned char)(128 + music_at(i, note) * 127 / peak);
    bcm_piezo_apply();
    piezo_play_samples(wav_buf, n, SAMPLE_RATE);
    return 0;
}

void bcm_piezo_shutdown(void)
{
    long end = current_tick + HZ;
    if (!global_settings.bcm_piezo_shutdown)
        return;
    piezo_play_tune(chime_down);
    sleep(HZ / 10);
    while (piezo_busy() && TIME_BEFORE(current_tick, end))
        sleep(HZ / 20);
}
#endif
