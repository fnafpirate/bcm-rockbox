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
/* bcm overlay v123 -- cab2cav: H.264 CABAC -> CAVLC entropy transcoder (bcm-rockbox).
 *
 * Rewrites a Main-profile stream that uses CABAC into the same stream coded with CAVLC, so that a
 * Baseline-only decoder (the iPod 5G/5.5G VideoCore h264dec) can play it.  Only the entropy coding
 * changes: every syntax element keeps its value, so the decoded pictures are bit-identical.
 *
 * Supported: progressive frames, I and P slices, any number of reference frames, one slice group,
 * 4:2:0 8-bit, 4x4 transform, I_PCM.  Refused (returns an error): B / SP / SI slices, interlace
 * (field pictures, MBAFF), weighted prediction, 8x8 transform / High profile, slice groups.
 *
 * Plain C, no allocation inside the library: the caller gives one work area.
 */
#ifndef BCM_CAB2CAV_H
#define BCM_CAB2CAV_H
#include <stdint.h>

#define C2C_MAX_MBS   1620                 /* level 3.0 frame size (720x576) */

enum {
    C2C_OK = 0,
    C2C_ERR_SYNTAX = -1,       /* broken / unexpected bitstream */
    C2C_ERR_UNSUPPORTED = -2,  /* B / SP / SI, interlace, weighted prediction, High tools */
    C2C_ERR_SPACE = -3,        /* output buffer too small */
    C2C_ERR_LEVEL = -4,        /* a coefficient CAVLC Main cannot code (level_prefix > 15) */
    C2C_ERR_NOPARAM = -5,      /* slice refers to a missing SPS / PPS */
};

typedef struct {
    uint32_t nals, slices, mbs, skipped;
    uint32_t bins;             /* CABAC bins decoded (decision + bypass + terminate) */
    uint32_t in_bytes, out_bytes;
    uint32_t clamped;          /* levels too large for CAVLC Main, written as the largest codable one */
    uint32_t escaped;          /* such levels written with level_prefix 16 (c2c_set_escape) */
} c2c_stats_t;

struct c2c;                    /* opaque */
typedef struct c2c c2c_t;

/* size of the work area; it must stay valid while the transcoder is used */
uint32_t c2c_state_size(void);
c2c_t *c2c_init(void *mem, uint32_t size);

/* Transcode one NAL unit.  `in` is the NAL unit without start code or length prefix (header byte
   first, emulation prevention bytes included); `out` receives the same (no start code).
   Returns the output length (> 0) or a C2C_ERR_* code.  Non-VCL NAL units are copied, except PPS
   (entropy_coding_mode_flag cleared).  `rbsp_tmp` is scratch of at least in_len + 16 bytes. */
int c2c_nal(c2c_t *c, const uint8_t *in, int in_len, uint8_t *out, int out_cap, uint8_t *rbsp_tmp);

const c2c_stats_t *c2c_stats(c2c_t *c);
const char *c2c_errstr(int err);

/* A CABAC stream can hold a coefficient level that CAVLC in Baseline / Main cannot code (|level| above ~2063,
   e.g. a large Intra 16x16 DC at a low QP).  By default it is written as the largest codable level (a small error in
   one block until the next key frame; counted in stats.clamped).  on = 1 writes it exactly with level_prefix 16, the
   High-profile escape -- only for a decoder that reads it. */
void c2c_set_escape(c2c_t *c, int on);

#endif
