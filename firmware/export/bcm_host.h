/*
 * bcm_host.h - VideoCore host interface ("ring channels") for the iPod Video 5.5G.
 *
 * What is verified vs inferred is tracked in VideoCore_Host_Interface_Session3.md.
 * Items marked [UNVERIFIED] below have never run against a real VideoCore.
 */
#ifndef BCM_HOST_H
#define BCM_HOST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- bring-up ---------------------------------------------------------------- */

/* Call after the VideoCore has been booted (bcm_init()).
 * Reads VC[0x1F0..0x1FF], takes the info pointer at VC[0x1FC] as the base of the host-interface
 * block, reads the channel directory and the channel descriptors.
 * The stock firmware requires VC[0x1F8] == 1 here; on real hardware under Rockbox that word holds
 * the legacy LCD command (0xFFFF0000), so it is only recorded (diag.ready_ok), not enforced.
 * Returns 0 on success, <0 on failure (-2: bad info pointer, -3: no gencmd channel,
 * -4: no plausible channel directory). */
int  bcm_host_attach(void);

/* What attach saw, for logging. Valid after any call to bcm_host_attach(). */
struct bcm_host_diag {
    uint32_t w1f0, w1f4, w1f8, w1fc;       /* VC[0x1F0..0x1FC] */
    bool     ready_ok;                     /* VC[0x1F8] == 1 (the stock firmware's condition) */
    uint16_t dir[8];                       /* channel directory at base (offsets from base) */
    uint16_t type[8];                      /* descriptor type per channel (0 = absent) */
    uint32_t tx_start[8], tx_end[8], rx_start[8], rx_end[8];   /* ring bounds, offsets from base */
};
const struct bcm_host_diag *bcm_host_get_diag(void);

/* Poll the VC->host signal byte and service every channel with pending messages.
 * There is no interrupt to imitate (see report 11.3): call at ~100-200 Hz from a thread. */
void bcm_host_service(void);

/* ---- channel 1: ASCII gencmd -------------------------------------------------- */

/* Sends `cmd` (header opcode 1, payload = string incl. NUL) and waits for the reply with the
 * same sequence number. The reply text is copied to `resp`. Returns the header result word
 * (>=0) or <0 on timeout / not attached. `max_polls` bounds the wait. */
int  bcm_gencmd(const char *cmd, char *resp, size_t resp_sz, int max_polls);

/* ---- channel 5: VCFS (VideoCore -> host file API, read-only) ------------------ */

/* Paths the VC sends start with "/Resources/". They are mapped to root + path.
 * Example: bcm_vfs_set_root("/.rockbox/vc") -> /.rockbox/vc/Resources/VideoCore/Library/h264dec.vll */
void bcm_vfs_set_root(const char *root);

/* ---- channel 7: VCPD (passthrough data stream: frames for the player) --------- */

struct bcm_pds_ops {
    /* opcode 0x41: VC announces two data buffers (VC RAM address + capacity) and a stream id
     * (decimal string at payload+0x420). w1 is the first payload word after word 0 [I: start flags]. */
    void (*start)(int stream_id, uint32_t w1);
    /* opcode 0x42: VC wants the next frame (or next chunk of the current one).
     * w2 = the stream id being asked for, -1 = any (the stock media object compares it with its
     * two stream ids, rt 0x20b39c) [I]; w1 is passed through as a third argument [meaning unknown]. */
    void (*get_frame)(int stream_id, uint32_t w1, uint32_t w2);
    /* opcode 0x43 */
    void (*seek)(int stream_id, uint32_t position);
    /* opcode 0x44 */
    void (*stop)(void);
};
void bcm_pds_set_ops(const struct bcm_pds_ops *ops);

/* Which VC buffer a stream id uses. 0 = buffer A (payload words 2/3 of 0x41), 1 = buffer B
 * (words 4/5). The stock firmware keeps this in a table at rt 0x10833c64 whose assignment I
 * have not found [UNVERIFIED]; default: stream 0 -> A, stream 1 -> B. */
void bcm_pds_map_stream(int stream_id, int buffer /*0=A,1=B*/);

struct bcm_pds_frame {
    /* Field origins in the stock media object (rt 0x20b39c): stream_id = the requested id, f4 =
     * frame-record word 2, f2 = low 16 bits of frame-record word 3, length = word 1, data = word 0.
     * meta[] are NOT written per frame by the stock producer; they look like per-stream constants
     * set elsewhere [I]. */
    int16_t  stream_id;
    uint16_t f2;          /* input descriptor +2  [meaning unknown; flags?] */
    uint32_t f4;          /* input descriptor +4  [meaning unknown; timestamp?] */
    uint32_t length;      /* frame bytes */
    uint32_t meta[4];     /* input descriptor +0x18..+0x24 [meaning unknown; timestamps/flags?] */
    const uint8_t *data;
};
/* Queue a frame. The first chunk is written to the VC buffer and announced immediately; the
 * remaining chunks (frames larger than the buffer) are sent on each following opcode 0x42.
 * Returns 0, or <0 if a previous frame on this buffer is still being sent / not attached. */
int  bcm_pds_send_frame(const struct bcm_pds_frame *f);

/* Answer a get_frame request with "nothing" (rt 0x20b688 calls the hand-off with (0,0): an 8-byte
 * 0x61 reply of two zero words). Presumably end-of-stream / no frame available [I]. */
int  bcm_pds_send_empty(void);

/* Chunk flag byte in the 0x61 reply (payload word 6). Values decoded from rt 0x288c14 but the
 * meaning is inferred from control flow [UNVERIFIED]. */
enum { PDS_FLAG_FIRST = 0, PDS_FLAG_MIDDLE = 1, PDS_FLAG_WHOLE = 2, PDS_FLAG_LAST = 3 };

/* ---- tracing ---------------------------------------------------------------------- */
/* Optional. Called for every VCFS operation, PDS opcode and any message on a channel the module
 * does not serve (VCHR, graphics), so first-hardware runs show what the VideoCore actually asks
 * for. `s` is a path or NULL. Do not call back into bcm_host from the callback. */
void bcm_host_set_trace(void (*fn)(const char *tag, uint32_t a, uint32_t b, const char *s));

/* ---- diagnostics ---------------------------------------------------------------- */
struct bcm_host_stats { uint32_t rx_msgs, tx_msgs, vcfs_ops, pds_ops, bad_magic, unknown_ops, truncated, resynced; };
const struct bcm_host_stats *bcm_host_get_stats(void);

#endif
