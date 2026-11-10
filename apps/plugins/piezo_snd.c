/***************************************************************************
 * bcm overlay v88: piezo_snd -- decodes the first seconds of any file Rockbox can play (MP3, M4A/AAC, FLAC, OGG, WMA,
 * WAV ...) to a 16 kHz mono 16-bit WAV, for the iPod's click speaker (apps/bcm_piezo.c: the start-up sound and the
 * charging chime from /.rockbox/startup chime and /.rockbox/charging chime).
 *
 * Rockbox itself runs it (parameter "source\ncache\nseconds"); started from the plugin menu it only says what it is for.
 * The codec plumbing is the one of test_codec.c (Dave Chapman, 2007) cut down: the file is read through the audio
 * buffer, the codec runs on the codec thread, its PCM comes back here, is mixed to mono, averaged down to 16 kHz (a box
 * filter is enough for a piezo disc) with any leading silence (up to 2 s) skipped, and written as a WAV.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later
 * version.
 ****************************************************************************/
#include "plugin.h"

#define OUT_RATE    16000
#define MAX_SECONDS 8
#define SILENCE     300                  /* |sample| below this (about -40 dB) at the start is skipped */

static int16_t out[MAX_SECONDS * OUT_RATE];
static unsigned out_n, out_max, skipped;
static bool started;
static long acc_sum; static int acc_cnt; static unsigned long acc_ph;

static struct codec_api ci;
static struct mp3entry id3;
static size_t filesize;
static int fd = -1;
static void *codec_mallocbuf, *audiobuf;
static size_t audiosize, audiobufsize;
static size_t offset;

static volatile bool codec_playing;
static volatile long codec_action;
static int in_rate = 44100, in_depth = 16, in_mode = STEREO_INTERLEAVED;

static void *codec_get_buffer(size_t *size) { *size = CODEC_SIZE; return codec_mallocbuf; }

static void put(long v)                  /* one input sample (16-bit scale) */
{
    acc_sum += v; acc_cnt++;
    acc_ph += OUT_RATE;
    if (acc_ph < (unsigned long)in_rate) return;
    acc_ph -= in_rate;
    v = acc_sum / acc_cnt;
    acc_sum = 0; acc_cnt = 0;
    if (!started)
    {
        if ((v < 0 ? -v : v) < SILENCE && skipped < 2 * OUT_RATE) { skipped++; return; }
        started = true;
    }
    if (out_n < out_max)
        out[out_n++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    if (out_n >= out_max)
        codec_action = CODEC_ACTION_HALT;
}

static void pcmbuf_insert(const void *ch1, const void *ch2, int count)
{
    int i, sh = in_depth > 16 ? in_depth - 15 : 0;
    rb->reset_poweroff_timer();
    if (out_n >= out_max) { codec_action = CODEC_ACTION_HALT; return; }
    for (i = 0; i < count && out_n < out_max; i++)
    {
        long l, r;
        if (in_depth > 16)
        {
            const int32_t *a = ch1, *b = ch2;
            if (in_mode == STEREO_INTERLEAVED) { l = a[2 * i]; r = a[2 * i + 1]; }
            else if (in_mode == STEREO_NONINTERLEAVED) { l = a[i]; r = b[i]; }
            else l = r = a[i];
            l >>= sh; r >>= sh;
        }
        else
        {
            const int16_t *a = ch1, *b = ch2;
            if (in_mode == STEREO_INTERLEAVED) { l = a[2 * i]; r = a[2 * i + 1]; }
            else if (in_mode == STEREO_NONINTERLEAVED) { l = a[i]; r = b[i]; }
            else l = r = a[i];
        }
        put((l + r) / 2);
    }
}

static int fill_buffer(size_t new_offset)
{
    size_t want = new_offset + audiobufsize <= filesize ? audiobufsize : filesize - new_offset;
    rb->lseek(fd, new_offset, SEEK_SET);
    if ((size_t)rb->read(fd, audiobuf, want) != want) return -1;
    offset = new_offset;
    return 0;
}

static void set_elapsed(unsigned long value) { ci.id3->elapsed = value; }

static size_t read_filebuf(void *ptr, size_t size)
{
    size_t realsize;
    if (ci.curpos >= (off_t)filesize) return 0;
    realsize = MIN(filesize - ci.curpos, size);
    if (realsize > audiobufsize - (ci.curpos - offset) && fill_buffer(ci.curpos) < 0) return 0;
    rb->memcpy(ptr, (char *)audiobuf + (ci.curpos - offset), realsize);
    ci.curpos += realsize;
    return realsize;
}

static void *request_buffer(size_t *realsize, size_t reqsize)
{
    *realsize = MIN(filesize - ci.curpos, reqsize);
    if (*realsize > audiobufsize - (ci.curpos - offset) && fill_buffer(ci.curpos) < 0) *realsize = 0;
    return (char *)audiobuf + (ci.curpos - offset);
}

static void advance_buffer(size_t amount) { ci.curpos += amount; ci.id3->offset = ci.curpos; }
static bool seek_buffer(size_t newpos) { ci.curpos = newpos; return true; }
static void seek_complete(void) { }
static long get_command(intptr_t *param) { (void)param; rb->yield(); return codec_action; }
static bool loop_track(void) { return false; }
static void set_offset(size_t value) { ci.id3->offset = value; }
static void strip_filesize(off_t size) { ci.filesize = size; }

static void configure(int setting, intptr_t value)
{
    switch (setting)
    {
    case DSP_SET_FREQUENCY:   if (value >= 4000) in_rate = (int)value; break;
    case DSP_SET_SAMPLE_DEPTH: in_depth = (int)value; break;
    case DSP_SET_STEREO_MODE: in_mode = (int)value; break;
    }
}

static void init_ci(void)
{
    ci.dsp = rb->dsp_get_config(CODEC_IDX_AUDIO);
    ci.codec_get_buffer = codec_get_buffer;
    ci.pcmbuf_insert = pcmbuf_insert;
    ci.set_elapsed = set_elapsed;
    ci.read_filebuf = read_filebuf;
    ci.request_buffer = request_buffer;
    ci.advance_buffer = advance_buffer;
    ci.seek_buffer = seek_buffer;
    ci.seek_complete = seek_complete;
    ci.set_offset = set_offset;
    ci.configure = configure;
    ci.get_command = get_command;
    ci.loop_track = loop_track;
    ci.strip_filesize = strip_filesize;
    ci.sleep = rb->sleep;
    ci.yield = rb->yield;
    ci.strcpy = rb->strcpy;
    ci.strlen = rb->strlen;
    ci.strcmp = rb->strcmp;
    ci.strcat = rb->strcat;
    ci.memset = rb->memset;
    ci.memcpy = rb->memcpy;
    ci.memmove = rb->memmove;
    ci.memcmp = rb->memcmp;
    ci.memchr = rb->memchr;
#if defined(DEBUG) || defined(SIMULATOR)
    ci.debugf = rb->debugf;
#endif
#ifdef ROCKBOX_HAS_LOGF
    ci.logf = rb->logf;
#endif
    ci.qsort = rb->qsort;
#ifdef RB_PROFILE
    ci.profile_thread = rb->profile_thread;
    ci.profstop = rb->profstop;
    ci.profile_func_enter = rb->profile_func_enter;
    ci.profile_func_exit = rb->profile_func_exit;
#endif
    ci.commit_dcache = rb->commit_dcache;
    ci.commit_discard_dcache = rb->commit_discard_dcache;
    ci.commit_discard_idcache = rb->commit_discard_idcache;
#if NUM_CORES > 1
    ci.create_thread = rb->create_thread;
    ci.thread_thaw = rb->thread_thaw;
    ci.thread_wait = rb->thread_wait;
    ci.semaphore_init = rb->semaphore_init;
    ci.semaphore_wait = rb->semaphore_wait;
    ci.semaphore_release = rb->semaphore_release;
#endif
#if defined(ARM_NEED_DIV0)
    ci.__div0 = rb->__div0;
#endif
}

static void codec_thread(void)
{
    if (rb->codec_load_file(rb->get_codec_filename(id3.codectype), &ci) >= 0)
        rb->codec_run_proc();
    rb->codec_close();
    codec_playing = false;
}

static void le32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static bool write_wav(const char *path)
{
    unsigned char h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
                            0,0,0,0, 0,0,0,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    unsigned i;
    int o;
    le32(h + 4, 36 + out_n * 2);
    le32(h + 24, OUT_RATE);
    le32(h + 28, OUT_RATE * 2);
    le32(h + 40, out_n * 2);
    for (i = 0; i < out_n; i++)                      /* little-endian on the iPod already; this keeps it portable */
    {
        uint16_t v = (uint16_t)out[i];
        ((unsigned char *)&out[i])[0] = v & 0xff;
        ((unsigned char *)&out[i])[1] = v >> 8;
    }
    o = rb->open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (o < 0) return false;
    if (rb->write(o, h, sizeof h) != (ssize_t)sizeof h || rb->write(o, out, out_n * 2) != (ssize_t)(out_n * 2))
    {
        rb->close(o);
        rb->remove(path);
        return false;
    }
    rb->close(o);
    return true;
}

enum plugin_status plugin_start(const void *parameter)
{
    char src[MAX_PATH], dst[MAX_PATH];
    const char *p = parameter, *nl, *nl2;
    int secs = 3;
    long t0;
    if (!p || !(nl = rb->strchr(p, '\n')))
    {
        rb->splash(HZ * 3, "Click speaker sound converter: Rockbox runs it by itself for /.rockbox/startup chime and "
                           "/.rockbox/charging chime");
        return PLUGIN_OK;
    }
    rb->strlcpy(src, p, MIN((size_t)(nl - p) + 1, sizeof src));
    nl2 = rb->strchr(nl + 1, '\n');
    rb->strlcpy(dst, nl + 1, nl2 ? MIN((size_t)(nl2 - nl), sizeof dst) : sizeof dst);
    if (nl2) secs = rb->atoi(nl2 + 1);
    if (secs < 1 || secs > MAX_SECONDS) secs = 3;
    out_max = (unsigned)secs * OUT_RATE;
    out_n = 0; skipped = 0; started = false; acc_sum = 0; acc_cnt = 0; acc_ph = 0;

    fd = rb->open(src, O_RDONLY);
    if (fd < 0) return PLUGIN_ERROR;
    filesize = rb->ffilesize(fd);
    rb->memset(&id3, 0, sizeof id3);
    if (!rb->get_metadata(&id3, fd, src)) { rb->close(fd); return PLUGIN_ERROR; }

    codec_mallocbuf = rb->plugin_get_audio_buffer(&audiosize);
    codec_mallocbuf = (void *)(((intptr_t)codec_mallocbuf + sizeof(intptr_t) - 1) & ~(sizeof(intptr_t) - 1));
    audiobuf = SKIPBYTES(codec_mallocbuf, CODEC_SIZE);
    audiosize -= CODEC_SIZE + sizeof(intptr_t);
    audiobufsize = filesize > audiosize ? audiosize : filesize;
    if (fill_buffer(0) < 0) { rb->close(fd); return PLUGIN_ERROR; }

    init_ci();
    ci.filesize = filesize;
    ci.id3 = &id3;
    ci.curpos = 0;
    in_rate = id3.frequency ? (int)id3.frequency : 44100;
    in_depth = 16; in_mode = STEREO_INTERLEAVED;

    rb->cpu_boost(true);
    codec_playing = true;
    codec_action = CODEC_ACTION_NULL;
    rb->codec_thread_do_callback(codec_thread, NULL);
    t0 = *rb->current_tick;
    while (codec_playing)
    {
        rb->sleep(HZ / 20);
        if (*rb->current_tick - t0 > 30 * HZ) codec_action = CODEC_ACTION_HALT;   /* a stuck codec: give up */
    }
    rb->codec_thread_do_callback(NULL, NULL);
    rb->cpu_boost(false);
    rb->close(fd);
    fd = -1;
    if (out_n < OUT_RATE / 20) return PLUGIN_ERROR;   /* under 50 ms of sound: nothing usable */
    return write_wav(dst) ? PLUGIN_OK : PLUGIN_ERROR;
}
