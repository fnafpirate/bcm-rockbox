/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * BCM2722 VideoCore host-interface driver for the iPod 5G/5.5G.
 *
 * Hardware layout:
 *   PP5022 (ARM7TDMI x2) ←16/32-bit bus→ BCM2722 VideoCore II
 *
 * The VideoCore exports a "host-interface block" whose base address is
 * stored at VC SRAM word 0x1FC (byte offset 0x7F0).  Each channel inside
 * the block has a descriptor that starts at (hib_base + channel_index*0x50)
 * and contains two ring-buffer pairs (TX and RX) plus signal bytes.
 *
 * Message format (every direction):
 *   [0]  magic  0xF1A55A1F
 *   [1]  sequence number (wrapping uint16)
 *   [2]  opcode (host→VC) or result (VC→host)
 *   [3]  payload byte length
 *   [4…] payload (zero-padded to multiple of 16 bytes)
 *
 * Ring protocol:
 *   TX_WR, TX_RD, RX_WR, RX_RD — four 32-bit pointers per channel
 *   Signal bytes at descriptor+0x10 (TX signal to VC) and +0x20 (RX)
 *   After every write to a signal byte: ring doorbell (BCM_CONTROL=0x31)
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

#include "bcm_host.h"
#include "bcm_host_io.h"
#include <string.h>
#include <stdio.h>

#ifdef IPOD_VIDEO
#include "file.h"
#include "logf.h"
#define BCM_LOG(...)  logf(__VA_ARGS__)
#else
#include <stdio.h>
#define BCM_LOG(...)  printf(__VA_ARGS__)
#endif

/* -----------------------------------------------------------------------
 * VC SRAM layout constants
 * --------------------------------------------------------------------- */
#define VC_SRAM_SIZE       0x140000u  /* 1.25 MB                             */
#define VC_OS_FLAG_OFF     0x7E0u     /* VC[0x1F8] in words = byte 0x7E0    */
#define VC_HIB_PTR_OFF     0x7F0u     /* VC[0x1FC] = host-interface base     */

/* Channel descriptor layout (each descriptor is 0x50 bytes) */
#define CHAN_DESC_SIZE     0x50u
#define CD_TX_WR           0x00u      /* TX write pointer (word offset)      */
#define CD_TX_RD           0x04u      /* TX read  pointer                    */
#define CD_RX_WR           0x08u      /* RX write pointer                    */
#define CD_RX_RD           0x0Cu      /* RX read  pointer                    */
#define CD_TX_SIG          0x10u      /* TX signal byte (host writes)        */
#define CD_RX_SIG          0x20u      /* RX signal byte (VC writes)          */
#define CD_TX_BUF_OFF      0x28u      /* TX ring buffer base (word offset)   */
#define CD_TX_BUF_SZ       0x2Cu      /* TX ring buffer size in bytes        */
#define CD_RX_BUF_OFF      0x30u      /* RX ring buffer base (word offset)   */
#define CD_RX_BUF_SZ       0x34u      /* RX ring buffer size in bytes        */

/* Message header words */
#define MSG_MAGIC          0xF1A55A1Fu
#define HDR_WORDS          4u
#define HDR_BYTES          (HDR_WORDS * 4u)

/* Maximum payload (VCFS replies can be up to 16 KB of data) */
#define MAX_PAYLOAD_BYTES  0x4010u

/* Bounded wait limit in ticks (≈ 15 × 10ms = 150ms) */
#define WAIT_TICKS         15

/* VCFS path utilities */
#define VCFS_DEFAULT_ROOT  "/.rockbox/VideoCore/"
#define VCFS_MAX_PATH      256

/* -----------------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------------- */
static bool     s_attached;
static uint32_t s_hib_base;   /* byte offset of host-interface block in VC SRAM */
static uint16_t s_seq[BCM_NUM_CHANS];

static char     s_vcfs_root[VCFS_MAX_PATH] = VCFS_DEFAULT_ROOT;

/* -----------------------------------------------------------------------
 * Low-level VC SRAM access via the BCM bus
 * --------------------------------------------------------------------- */
uint32_t bcm_vc_read(uint32_t off)
{
    bcm_io_lock();
    bcm_io_write(BCM_RD_ADDR32, off >> 2);
    uint32_t v = bcm_io_read(BCM_DATA);
    bcm_io_unlock();
    return v;
}

void bcm_vc_write(uint32_t off, uint32_t val)
{
    bcm_io_lock();
    bcm_io_write(BCM_WR_ADDR32, off >> 2);
    bcm_io_write(BCM_DATA, val);
    bcm_io_unlock();
}

/* -----------------------------------------------------------------------
 * Channel descriptor helpers
 * --------------------------------------------------------------------- */
static uint32_t chan_desc_base(int chan)
{
    return s_hib_base + (uint32_t)chan * CHAN_DESC_SIZE;
}

static uint32_t cd_read(int chan, uint32_t field)
{
    return bcm_vc_read(chan_desc_base(chan) + field);
}

static void cd_write(int chan, uint32_t field, uint32_t val)
{
    bcm_vc_write(chan_desc_base(chan) + field, val);
}

/* Signal the VC on a channel (write signal byte then ring doorbell). */
static void chan_kick(int chan, uint8_t sig_val)
{
    uint32_t base = chan_desc_base(chan);
    /* Signal byte is at a byte offset; we do a full 32-bit write centred
     * on the word that contains it, preserving the other bytes. */
    uint32_t word_off = (base + CD_TX_SIG) & ~3u;
    uint32_t byte_pos = (base + CD_TX_SIG) & 3u;
    uint32_t w = bcm_vc_read(word_off);
    w &= ~(0xFFu << (byte_pos * 8u));
    w |= ((uint32_t)sig_val << (byte_pos * 8u));
    bcm_vc_write(word_off, w);
    bcm_io_doorbell();
}

/* -----------------------------------------------------------------------
 * Ring-buffer write (host → VC, TX ring)
 * --------------------------------------------------------------------- */
static int ring_write(int chan,
                      const uint32_t *words, uint32_t nwords,
                      uint8_t sig_after)
{
    uint32_t buf_off  = cd_read(chan, CD_TX_BUF_OFF);  /* byte offset        */
    uint32_t buf_sz   = cd_read(chan, CD_TX_BUF_SZ);   /* ring size in bytes */
    uint32_t wr       = cd_read(chan, CD_TX_WR);        /* word index into ring */
    uint32_t buf_words = buf_sz / 4u;

    if (!buf_words) return -1;

    for (uint32_t i = 0; i < nwords; i++)
    {
        bcm_vc_write(buf_off + wr * 4u, words[i]);
        wr = (wr + 1u) % buf_words;
    }

    cd_write(chan, CD_TX_WR, wr);
    chan_kick(chan, sig_after);
    return 0;
}

/* -----------------------------------------------------------------------
 * Ring-buffer read (VC → host, RX ring)
 * --------------------------------------------------------------------- */
static uint32_t ring_avail(int chan)
{
    uint32_t buf_sz   = cd_read(chan, CD_RX_BUF_SZ);
    uint32_t buf_words = buf_sz / 4u;
    if (!buf_words) return 0;
    uint32_t wr = cd_read(chan, CD_RX_WR);
    uint32_t rd = cd_read(chan, CD_RX_RD);
    return (wr - rd + buf_words) % buf_words;
}

static int ring_read(int chan, uint32_t *words, uint32_t nwords)
{
    uint32_t buf_off   = cd_read(chan, CD_RX_BUF_OFF);
    uint32_t buf_sz    = cd_read(chan, CD_RX_BUF_SZ);
    uint32_t rd        = cd_read(chan, CD_RX_RD);
    uint32_t buf_words = buf_sz / 4u;

    if (!buf_words) return -1;

    for (uint32_t i = 0; i < nwords; i++)
    {
        words[i] = bcm_vc_read(buf_off + rd * 4u);
        rd = (rd + 1u) % buf_words;
    }

    /* Publish the updated read pointer and signal the VC (v18: must signal
     * after EVERY partial read so the VC knows we have consumed data) */
    cd_write(chan, CD_RX_RD, rd);
    chan_kick(chan, 1);
    return 0;
}

/* -----------------------------------------------------------------------
 * Bounded wait for N words to arrive on the RX ring
 * Returns 0 if data arrived in time, -1 on timeout.
 * --------------------------------------------------------------------- */
static int ring_wait(int chan, uint32_t nwords)
{
    uint32_t deadline = bcm_io_current_tick() + WAIT_TICKS;
    while (ring_avail(chan) < nwords)
    {
        if ((int32_t)(bcm_io_current_tick() - deadline) >= 0)
            return -1;
        bcm_io_sleep_ticks(1);
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Send a complete message on a channel (builds header + payload)
 * --------------------------------------------------------------------- */
static int msg_send(int chan, uint32_t opcode,
                    const uint32_t *payload, uint32_t payload_bytes)
{
    /* Round payload up to 16-byte boundary */
    uint32_t padded = (payload_bytes + 15u) & ~15u;
    uint32_t total_words = HDR_WORDS + padded / 4u;

    /* Allocate a small header + payload staging area on the stack.
     * MAX_PAYLOAD_BYTES is 0x4010; stay within reason for the stack. */
    if (padded > MAX_PAYLOAD_BYTES) return -1;

    uint32_t buf[(HDR_WORDS + MAX_PAYLOAD_BYTES / 4u)];
    buf[0] = MSG_MAGIC;
    buf[1] = (uint32_t)(s_seq[chan]++ & 0xFFFFu);
    buf[2] = opcode;
    buf[3] = payload_bytes;

    if (payload && payload_bytes)
        memcpy(&buf[HDR_WORDS], payload, payload_bytes);
    if (padded > payload_bytes)
        memset((uint8_t *)&buf[HDR_WORDS] + payload_bytes, 0,
               padded - payload_bytes);

    return ring_write(chan, buf, total_words, 1);
}

/* -----------------------------------------------------------------------
 * Receive a complete message on a channel
 * Fills header_out[4] and up to reply_cap bytes of payload into reply.
 * *reply_len is set to the actual payload length.
 * --------------------------------------------------------------------- */
static int msg_recv(int chan,
                    uint32_t *header_out,
                    uint8_t  *reply, size_t reply_cap,
                    size_t   *reply_len)
{
    /* Wait for the header */
    if (ring_wait(chan, HDR_WORDS) < 0) return -1;

    uint32_t hdr[HDR_WORDS];
    ring_read(chan, hdr, HDR_WORDS);

    if (hdr[0] != MSG_MAGIC) return -2;

    uint32_t payload_bytes = hdr[3];
    if (header_out) memcpy(header_out, hdr, HDR_WORDS * 4u);

    /* Round up to words */
    uint32_t payload_words = (payload_bytes + 3u) / 4u;
    if (reply_len) *reply_len = payload_bytes;

    /* Read payload in chunks, publishing RX_RD and signalling after each
     * chunk so the VC never starves (v18 fix). */
    uint32_t tmp[MAX_PAYLOAD_BYTES / 4u];
    uint32_t remaining = payload_words;
    uint32_t dst_word  = 0;

    while (remaining)
    {
        /* Wait for at least one word */
        if (ring_wait(chan, 1) < 0) return -3;

        uint32_t avail = ring_avail(chan);
        if (avail > remaining) avail = remaining;

        ring_read(chan, &tmp[dst_word], avail);
        dst_word  += avail;
        remaining -= avail;
    }

    if (reply && reply_cap && payload_bytes)
    {
        size_t copy = payload_bytes < reply_cap ? payload_bytes : reply_cap;
        memcpy(reply, tmp, copy);
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * bcm_host_attach
 * --------------------------------------------------------------------- */
int bcm_host_attach(void)
{
    s_attached = false;

    /* VC[0x1F8] was=1 at OS start but Rockbox's legacy LCD driver
     * overwrites it immediately.  Only trust VC[0x1FC] (HIB pointer). */
    uint32_t hib_ptr = bcm_vc_read(VC_HIB_PTR_OFF);

    /* Sanity: must be inside VC SRAM and word-aligned */
    if (hib_ptr == 0 || hib_ptr & 3u || hib_ptr >= VC_SRAM_SIZE)
        return -1;

    s_hib_base = hib_ptr;

    /* Verify at least BCM_NUM_CHANS channel descriptors look plausible */
    for (int c = 0; c < BCM_NUM_CHANS; c++)
    {
        uint32_t buf_off = cd_read(c, CD_RX_BUF_OFF);
        uint32_t buf_sz  = cd_read(c, CD_RX_BUF_SZ);
        if (buf_sz == 0 || (buf_sz & 3u) || buf_off >= VC_SRAM_SIZE)
            return -2;
    }

    memset(s_seq, 0, sizeof(s_seq));
    s_attached = true;
    BCM_LOG("bcm_host: attached, hib=0x%08lx", (unsigned long)s_hib_base);
    return 0;
}

bool bcm_host_alive(void)
{
    if (!s_attached) return false;
    /* Quick liveness: check that HIB pointer is still valid */
    uint32_t p = bcm_vc_read(VC_HIB_PTR_OFF);
    return (p == s_hib_base);
}

/* -----------------------------------------------------------------------
 * gencmd
 * --------------------------------------------------------------------- */
int bcm_gencmd(const char *cmd, char *reply, size_t reply_size)
{
    if (!s_attached) return -1;

    size_t cmd_len = strlen(cmd) + 1u;  /* include NUL */
    int r = msg_send(BCM_CHAN_GENCMD, BCM_OP_GENCMD,
                     (const uint32_t *)cmd,
                     (uint32_t)((cmd_len + 3u) & ~3u));
    if (r < 0) return r;

    uint32_t hdr[HDR_WORDS];
    size_t   got;
    r = msg_recv(BCM_CHAN_GENCMD, hdr, (uint8_t *)reply, reply_size, &got);
    if (r < 0) return r;

    /* Ensure NUL termination */
    if (reply && reply_size)
        reply[got < reply_size ? got : reply_size - 1] = '\0';

    return (int)hdr[2];  /* result code */
}

/* -----------------------------------------------------------------------
 * Graphics channel
 * --------------------------------------------------------------------- */
int bcm_gfx_send(uint32_t opcode,
                 const uint32_t *args, int nargs,
                 uint32_t *result)
{
    if (!s_attached) return -1;

    int r = msg_send(BCM_CHAN_GFX, opcode,
                     args, (uint32_t)(nargs * 4));
    if (r < 0) return r;

    uint32_t hdr[HDR_WORDS];
    size_t   got;
    uint32_t res_buf[64];
    r = msg_recv(BCM_CHAN_GFX, hdr, (uint8_t *)res_buf, sizeof(res_buf), &got);
    if (r < 0) return r;

    if (result && got >= 4)
        *result = res_buf[0];

    return (int)hdr[2];
}

/* -----------------------------------------------------------------------
 * Stream (VCPD) channel
 * --------------------------------------------------------------------- */
int bcm_stream_send(uint32_t opcode,
                    const uint32_t *payload, size_t payload_len,
                    uint32_t *reply, size_t reply_cap, size_t *reply_len,
                    bool wait_reply)
{
    if (!s_attached) return -1;

    int r = msg_send(BCM_CHAN_STREAM, opcode,
                     payload, (uint32_t)payload_len);
    if (r < 0) return r;

    if (!wait_reply)
    {
        if (reply_len) *reply_len = 0;
        return 0;
    }

    uint32_t hdr[HDR_WORDS];
    return msg_recv(BCM_CHAN_STREAM, hdr,
                    (uint8_t *)reply, reply_cap, reply_len);
}

/* -----------------------------------------------------------------------
 * VCFS server — handles open/close/seek/read requests from the VC
 * --------------------------------------------------------------------- */
void bcm_vcfs_set_root(const char *root)
{
    strncpy(s_vcfs_root, root, sizeof(s_vcfs_root) - 1);
    s_vcfs_root[sizeof(s_vcfs_root) - 1] = '\0';
}

/* Convert a VC path (backslashes, no .vll extension, may be relative)
 * to a Rockbox host path under s_vcfs_root.
 * Returns the host path in @out (size @out_sz). */
static void vcfs_map_path(const char *vc_path, char *out, size_t out_sz,
                          bool add_vll)
{
    snprintf(out, out_sz, "%s", s_vcfs_root);
    size_t root_len = strlen(out);
    char *dst = out + root_len;
    size_t rem = out_sz - root_len - 1u;

    for (const char *p = vc_path; *p && rem; p++, dst++, rem--)
        *dst = (*p == '\\') ? '/' : *p;
    *dst = '\0';

    if (add_vll)
    {
        size_t len = strlen(out);
        if (len + 4u < out_sz)
            strcpy(out + len, ".vll");
    }
}

/* Handle one VCFS inbound message.  Called from bcm_host_service(). */
static void vcfs_handle(uint32_t opcode, const uint8_t *req, uint32_t req_len)
{
    uint8_t  resp[MAX_PAYLOAD_BYTES];
    uint32_t resp_len = 0;

    switch (opcode)
    {
    case BCM_VCFS_OPEN:
    {
        /* Payload: uint32 handle_hint, then NUL-terminated path */
        if (req_len < 5) break;
        uint32_t hint;
        memcpy(&hint, req, 4);
        const char *vc_path = (const char *)(req + 4);

        char host_path[VCFS_MAX_PATH];
        vcfs_map_path(vc_path, host_path, sizeof(host_path), false);

#ifdef IPOD_VIDEO
        int fd = open(host_path, O_RDONLY);
        if (fd < 0)
        {
            /* Retry with .vll appended */
            vcfs_map_path(vc_path, host_path, sizeof(host_path), true);
            fd = open(host_path, O_RDONLY);
        }
        uint32_t result_fd = (fd >= 0) ? (uint32_t)fd : (uint32_t)-1;
        BCM_LOG("vcfs open '%s' → fd %d", host_path, fd);
#else
        /* PC test: always succeed with fd = hint */
        uint32_t result_fd = hint;
#endif
        memcpy(resp, &result_fd, 4);
        resp_len = 4;
        break;
    }

    case BCM_VCFS_CLOSE:
    {
        if (req_len < 4) break;
        uint32_t fd;
        memcpy(&fd, req, 4);
#ifdef IPOD_VIDEO
        close((int)fd);
#endif
        resp_len = 0;
        break;
    }

    case BCM_VCFS_SEEK:
    {
        if (req_len < 12) break;
        uint32_t fd, pos_lo, whence;
        memcpy(&fd,     req + 0,  4);
        memcpy(&pos_lo, req + 4,  4);
        memcpy(&whence, req + 8,  4);
#ifdef IPOD_VIDEO
        lseek((int)fd, (off_t)pos_lo, (int)whence);
#endif
        resp_len = 0;
        break;
    }

    case BCM_VCFS_READ:
    {
        if (req_len < 8) break;
        uint32_t fd, nbytes;
        memcpy(&fd,     req + 0, 4);
        memcpy(&nbytes, req + 4, 4);
        if (nbytes > MAX_PAYLOAD_BYTES) nbytes = MAX_PAYLOAD_BYTES;
#ifdef IPOD_VIDEO
        ssize_t got = read((int)fd, resp, nbytes);
        resp_len = (got > 0) ? (uint32_t)got : 0;
#else
        resp_len = 0;
#endif
        break;
    }

    default:
        BCM_LOG("vcfs: unknown opcode 0x%02x", (unsigned)opcode);
        break;
    }

    /* Send reply */
    msg_send(BCM_CHAN_VCFS, opcode, (const uint32_t *)resp, resp_len);
}

/* -----------------------------------------------------------------------
 * VCHR handler — handles event notifications and stubs from the VC
 * --------------------------------------------------------------------- */
static void vchr_handle(uint32_t opcode,
                        const uint8_t *req, uint32_t req_len)
{
    (void)req; (void)req_len;

    switch (opcode)
    {
    case BCM_VCHR_EVENT:
        /* v13-v27 bug: replying with 4 bytes halted the VC.
         * Fix (v28): reply with ZERO-length payload. */
        msg_send(BCM_CHAN_VCHR, opcode, NULL, 0);
        break;

    case BCM_VCHR_STUB:
        /* Stub: return 0 */
    {
        uint32_t zero = 0;
        msg_send(BCM_CHAN_VCHR, opcode, &zero, 4);
        break;
    }

    default:
        BCM_LOG("vchr: unhandled opcode 0x%02x", (unsigned)opcode);
        break;
    }
}

/* -----------------------------------------------------------------------
 * bcm_host_service — process one pending inbound message
 * --------------------------------------------------------------------- */
int bcm_host_service(void)
{
    if (!s_attached) return -1;

    int processed = 0;

    /* Check VCFS and VCHR for pending messages */
    int inbound_chans[] = { BCM_CHAN_VCFS, BCM_CHAN_VCHR };
    for (int ci = 0; ci < 2; ci++)
    {
        int chan = inbound_chans[ci];
        if (ring_avail(chan) < HDR_WORDS) continue;

        uint32_t hdr[HDR_WORDS];
        uint8_t  payload[MAX_PAYLOAD_BYTES];
        size_t   payload_len;

        int r = msg_recv(chan, hdr, payload, sizeof(payload), &payload_len);
        if (r < 0) continue;

        uint32_t opcode = hdr[2];

        if (chan == BCM_CHAN_VCFS)
            vcfs_handle(opcode, payload, (uint32_t)payload_len);
        else
            vchr_handle(opcode, payload, (uint32_t)payload_len);

        processed = 1;
    }

    return processed;
}
