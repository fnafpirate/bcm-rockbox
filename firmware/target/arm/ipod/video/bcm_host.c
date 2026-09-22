/*
 * bcm_host.c - VideoCore host interface for Rockbox on the iPod Video 5.5G (PP5022 + BCM2722)
 *
 * Everything below is derived from disassembly of Firmware-25.6.3 (osos runtime addresses,
 * file offset - 0x5000) plus the qemu-ipod-classic wiki and openVC02. Function addresses in
 * the comments are osos runtime addresses ("rt").
 *
 *   attach     rt 0x288058   read VC[0x1F0..0x1FF]; VC[0x1F8] must be 1; VC[0x1FC] = base;
 *                            read 8 x u16 directory at base, then the 0x50-byte descriptors
 *   ring write rt 0x28871c   TX ring, host owns TX_WR (+0x20), VC owns TX_RD (+0x10), 16-byte guard
 *   ring read  rt 0x288434   RX ring, VC owns RX_WR (+0x40), host owns RX_RD (+0x30)
 *   compose    rt 0x28861c   header {magic, seq, opcode, u16 len, u16 0} + payload padded to 16
 *   signal     rt 0x28821c   toggle bit `channel` of header byte +0x10 unless already pending;
 *                            VC acks in +0x20; VC->host signal is +0x21, host acks in +0x11
 *   VCFS       rt 0xf9600    open 0x4c / close 0x41 / seek 0x43 / read 0x44
 *   VCPD       rt 0xff034    opcodes 0x40..0x44, frame reply 0x61 built at rt 0x288c14
 *
 * Endianness: ARM and VideoCore are little-endian; the code memcpy()s fields and assumes a
 * little-endian host (true for the target and for x86 test hosts).
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include "bcm_host_io.h"
#include "bcm_host.h"

#ifdef BCM_HOST_TEST
#include <unistd.h>
#include <stdio.h>
#else
#include "file.h"
#endif

#ifndef BCM_LOG
#define BCM_LOG(...) do { } while (0)
#endif

/* ------------------------------------------------------------------ layout ---- */
#define HI_MAGIC          0xF1A55A1Fu
#define HI_NCHAN          8
#define HI_HDR            16u                 /* message header size */
#define HI_MAX_PAYLOAD    0x4010u             /* 16 KiB of file data + the 16-byte VCFS parameter block */

#define H_TX_SIG          0x10                /* host -> VC signal byte             */
#define H_RX_ACK          0x11                /* host ack of VC signals             */
#define H_TX_ACK          0x20                /* VC ack of host signals             */
#define H_RX_SIG          0x21                /* VC -> host signal byte             */

#define D_SIZE            0x50
#define D_TYPE            0x04
#define D_TX_START        0x06
#define D_TX_END          0x08
#define D_RX_START        0x0a
#define D_RX_END          0x0c
#define D_TX_RD           0x10                /* VC-owned line   */
#define D_TX_WR           0x20                /* host-owned line */
#define D_RX_RD           0x30                /* host-owned line */
#define D_RX_WR           0x40                /* VC-owned line   */

#define CH_GENCMD         1
#define CH_GRAPHICS       2
#define CH_VCFS           5
#define CH_VCHR           6
#define CH_VCPD           7

#define GENCMD_OPCODE     1                   /* rt 0xe93c0: compose(ch, 1, strlen+1, str) */

/* VCFS opcodes (rt 0xf974c jump table) */
#define VCFS_CLOSE        0x41
#define VCFS_SEEK         0x43
#define VCFS_READ         0x44
#define VCFS_OPEN         0x4c

/* VCPD opcodes (rt 0xff034) */
#define PDS_PING          0x40
#define PDS_START         0x41
#define PDS_GET_FRAME     0x42
#define PDS_SEEK          0x43
#define PDS_STOP          0x44
#define PDS_FRAME_REPLY   0x61                /* rt 0x288e.. : header opcode of the 12-word reply */

struct hi_chan {
    bool     present;
    uint16_t type;
    uint16_t desc;                            /* descriptor offset from base */
    uint16_t tx_start, tx_end, rx_start, rx_end;
    uint16_t tx_rd, tx_wr, rx_rd, rx_wr;      /* shadows */
    uint32_t seq;                             /* host request sequence counter */
};

struct hi_msg {
    uint32_t seq, op;
    uint16_t len;
    uint8_t  payload[HI_MAX_PAYLOAD + 16];
};

static struct {
    bool     attached;
    uint32_t base;
    uint8_t  tx_sig;                          /* shadow of +0x10 */
    uint8_t  rx_ack;                          /* shadow of +0x11 */
    struct hi_chan ch[HI_NCHAN];
    /* gencmd */
    bool     gc_wait;  uint32_t gc_seq;  bool gc_done;  int gc_result;
    char     *gc_resp; size_t gc_resp_sz;
    /* VCFS */
    char     vfs_root[96];
    int      vfs_fd[8];
    /* PDS */
    const struct bcm_pds_ops *pds_ops;
    int      pds_stream_buf[4];
    uint32_t pds_last_seq;                    /* seq echoed in frame replies (rt chan+0x18) */
    int      pds_cur_stream;
    struct { uint32_t addr, cap; } pds_buf[2];
    struct {
        bool active; struct bcm_pds_frame f; uint32_t sent; uint32_t counter;
    } pds_pend[2];
    struct bcm_host_stats st;
    void (*trace)(const char *, uint32_t, uint32_t, const char *);
} hi;

#define TRACE(tag, a, b, s) do { if (hi.trace) hi.trace((tag), (a), (b), (s)); } while (0)
void bcm_host_set_trace(void (*fn)(const char *, uint32_t, uint32_t, const char *)) { hi.trace = fn; }

const struct bcm_host_stats *bcm_host_get_stats(void) { return &hi.st; }
static struct bcm_host_diag diag;
const struct bcm_host_diag *bcm_host_get_diag(void) { return &diag; }

/* ------------------------------------------------------------- small helpers -- */
static uint32_t rd32(uint32_t addr) { uint32_t v; bcm_io_read(addr, &v, 4); return v; }
static void     wr32(uint32_t addr, uint32_t v) { bcm_io_write(addr, &v, 4); }

static uint16_t rd16(uint32_t addr)
{
    uint32_t w = rd32(addr & ~3u);
    return (addr & 2) ? (uint16_t)(w >> 16) : (uint16_t)w;
}
static void wr16(uint32_t addr, uint16_t v)
{
    uint32_t w = rd32(addr & ~3u);
    if (addr & 2) w = (w & 0x0000ffffu) | ((uint32_t)v << 16);
    else          w = (w & 0xffff0000u) | v;
    wr32(addr & ~3u, w);
}
static uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void     st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t ld16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* rt 0xf5834: bytes in a ring */
static uint32_t ring_used(uint16_t start, uint16_t end, uint16_t rd, uint16_t wr)
{
    return (wr < rd) ? (uint32_t)(end - rd) + (uint32_t)(wr - start) : (uint32_t)(wr - rd);
}

static struct hi_chan *chan_by_type(uint16_t type)
{
    for (int i = 0; i < HI_NCHAN; i++)
        if (hi.ch[i].present && hi.ch[i].type == type) return &hi.ch[i];
    return NULL;
}
static int chan_index(const struct hi_chan *c) { return (int)(c - hi.ch); }

/* ------------------------------------------------------------------- attach --- */
int bcm_host_attach(void)
{
    uint32_t w[4];
    memset(&hi.st, 0, sizeof hi.st);
    memset(&diag, 0, sizeof diag);
    hi.attached = false;
    bcm_io_lock();
    bcm_io_read(0x1F0, w, 16);
    diag.w1f0 = w[0]; diag.w1f4 = w[1]; diag.w1f8 = w[2]; diag.w1fc = w[3];
    diag.ready_ok = (w[2] == 1);
    hi.base = w[3];
    if (hi.base == 0 || (hi.base & 3)) { bcm_io_unlock(); BCM_LOG("bad info pointer %08x", hi.base); return -2; }

    uint16_t dir[HI_NCHAN];
    bcm_io_read(hi.base, dir, 16);
    memcpy(diag.dir, dir, sizeof dir);
    uint32_t hdr = rd32(hi.base + H_TX_SIG);
    hi.tx_sig = (uint8_t)hdr;
    hi.rx_ack = (uint8_t)(hdr >> 8);

    int present = 0;
    for (int i = 0; i < HI_NCHAN; i++) {
        struct hi_chan *c = &hi.ch[i];
        memset(c, 0, sizeof *c);
        if (dir[i] == 0 || (dir[i] & 3)) continue;               /* empty or misaligned entry (real offsets reach 0x5740) */
        uint8_t d[D_SIZE];
        bcm_io_read(hi.base + dir[i], d, D_SIZE);
        c->desc     = dir[i];
        c->type     = ld16(d + D_TYPE);
        c->tx_start = ld16(d + D_TX_START);  c->tx_end = ld16(d + D_TX_END);
        c->rx_start = ld16(d + D_RX_START);  c->rx_end = ld16(d + D_RX_END);
        c->tx_rd    = ld16(d + D_TX_RD);     c->tx_wr  = ld16(d + D_TX_WR);
        c->rx_rd    = ld16(d + D_RX_RD);     c->rx_wr  = ld16(d + D_RX_WR);
        diag.type[i] = c->type; diag.tx_start[i] = c->tx_start; diag.rx_start[i] = c->rx_start;
        diag.tx_end[i] = c->tx_end; diag.rx_end[i] = c->rx_end;
        /* plausible: a small type number and rings with start < end */
        if (c->type >= 1 && c->type <= 15 && c->tx_start < c->tx_end && c->rx_start < c->rx_end) {
            c->present = true;
            present++;
        }
    }
    bcm_io_unlock();

    if (present == 0) return -4;
    for (int i = 0; i < 8; i++) hi.vfs_fd[i] = -1;
    hi.pds_stream_buf[0] = 0; hi.pds_stream_buf[1] = 1;
    hi.pds_stream_buf[2] = hi.pds_stream_buf[3] = 0;
    hi.pds_cur_stream = -1;
    memset(hi.pds_pend, 0, sizeof hi.pds_pend);
    if (!chan_by_type(CH_GENCMD)) return -3;
    hi.attached = true;
    return 0;
}

/* ------------------------------------------------------------------- rings ---- */
/* rt 0x28871c ring_write: returns bytes written (may be < len when the ring is full). */
static uint32_t ring_write(struct hi_chan *c, const uint8_t *src, uint32_t len)
{
    uint32_t written = 0;
    uint16_t lim = (c->tx_start == c->tx_rd) ? c->tx_end : c->tx_rd;
    int32_t  max = (int32_t)lim - 0x10;
    if ((int32_t)c->tx_wr > max) {                       /* free area wraps: fill up to end */
        uint32_t n = c->tx_end - c->tx_wr;
        if (n > len) n = len;
        bcm_io_write(hi.base + c->tx_wr, src, n);
        c->tx_wr += n; len -= n; src += n; written += n;
        if (c->tx_wr == c->tx_end) c->tx_wr = c->tx_start;
    }
    if ((int32_t)c->tx_wr < max) {
        uint32_t n = (uint32_t)(max - c->tx_wr);
        if (n > len) n = len;
        if (n) {
            bcm_io_write(hi.base + c->tx_wr, src, n);
            c->tx_wr += n; written += n;
        }
    }
    return written;
}

/* publish TX_WR and toggle the channel signal bit (rt 0x288800 + 0x28821c) */
static void chan_kick(struct hi_chan *c)
{
    wr16(hi.base + c->desc + D_TX_WR, c->tx_wr);
    uint32_t w = rd32(hi.base + H_TX_ACK);                /* bytes +0x20 (ack), +0x21 */
    uint8_t ack = (uint8_t)w;
    uint8_t bit = (uint8_t)(1u << chan_index(c));
    uint8_t pend = hi.tx_sig ^ ack;
    hi.tx_sig ^= (uint8_t)(bit & ~pend);                  /* toggle only if not already pending */
    uint32_t h = rd32(hi.base + H_TX_SIG);
    h = (h & 0xffff0000u) | hi.tx_sig | ((uint32_t)hi.rx_ack << 8);
    wr32(hi.base + H_TX_SIG, h);
    bcm_io_doorbell();
}

/* Send one message, waiting (bounded) for ring space. Returns 0 or -1. */
static int hi_tx(struct hi_chan *c, uint32_t op, uint32_t seq, const void *payload, uint32_t len)
{
    static uint8_t buf[HI_HDR + HI_MAX_PAYLOAD + 16];   /* static: single service thread, keeps the stack small */
    uint32_t padded = (len + 15u) & ~15u;
    if (len > HI_MAX_PAYLOAD) return -1;
    memset(buf, 0, HI_HDR + padded);
    st32(buf + 0, HI_MAGIC);
    st32(buf + 4, seq);
    st32(buf + 8, op);
    { uint16_t l = (uint16_t)len; memcpy(buf + 12, &l, 2); }
    if (len) memcpy(buf + HI_HDR, payload, len);

    uint32_t total = HI_HDR + padded, done = 0;
    int spins = 0;
    bcm_io_lock();
    while (done < total) {
        uint32_t n = ring_write(c, buf + done, total - done);
        done += n;
        if (n) { chan_kick(c); spins = 0; }
        if (done < total) {                               /* ring full: wait for the VC to consume */
            bcm_io_unlock();
            bcm_io_idle();
            bcm_io_lock();
            c->tx_rd = rd16(hi.base + c->desc + D_TX_RD); /* rt 0x288928 refresh */
            if (++spins > 1000) { bcm_io_unlock(); return -1; }   /* ~10 s at sleep(1) per idle */
        }
    }
    bcm_io_unlock();
    hi.st.tx_msgs++;
    return 0;
}

/* rt 0x288434 ring_read */
static uint32_t ring_read(struct hi_chan *c, uint8_t *dst, uint32_t len)
{
    uint32_t got = 0;
    if (c->rx_wr < c->rx_rd) {
        uint32_t n = c->rx_end - c->rx_rd;
        if (n > len) n = len;
        bcm_io_read(hi.base + c->rx_rd, dst, n);
        len -= n; dst += n; got += n; c->rx_rd += n;
        if (c->rx_rd == c->rx_end) c->rx_rd = c->rx_start;
    }
    if (c->rx_wr > c->rx_rd) {
        uint32_t n = c->rx_wr - c->rx_rd;
        if (n > len) n = len;
        if (n) {
            bcm_io_read(hi.base + c->rx_rd, dst, n);
            got += n; c->rx_rd += n;
        }
    }
    return got;
}

/* Pop one complete message. Returns 1 if a message was read, 0 if none.
 *
 * v5 tried to WAIT for the rest of a message that did not fit the ring in one write (a plausible
 * design, matching how the host's own hi_tx() waits for space). On real hardware this hung for
 * ~5s and then desynchronised every later reply on the channel (session 3, run 5: `commands` and
 * `set_vll_dir` both timed out, bad_magic=5). The VC does not appear to top a reply up after the
 * fact -- it either fits or it does not -- so v6 does not wait: it takes whatever is in the ring
 * right now and, if that is less than the header's declared length, delivers it as a truncated
 * message (dropping the unwritten tail) rather than discarding it and stalling the caller. This
 * matches the one known real case (a ~540-byte `commands` list through a 512-byte ring) and never
 * blocks, so a genuinely bad header (bogus length) costs one truncated message, not a 5s hang. */
static int hi_rx_pop(struct hi_chan *c, struct hi_msg *m)
{
    bcm_io_lock();
    c->rx_wr = rd16(hi.base + c->desc + D_RX_WR);         /* VC-owned line refresh */
    uint32_t avail = ring_used(c->rx_start, c->rx_end, c->rx_rd, c->rx_wr);
    if (avail < HI_HDR) { bcm_io_unlock(); return 0; }

    uint8_t hdr[HI_HDR];
    ring_read(c, hdr, HI_HDR);
    if (ld32(hdr) != HI_MAGIC) {
        hi.st.bad_magic++;
        wr16(hi.base + c->desc + D_RX_RD, c->rx_rd);
        bcm_io_unlock();
        return 0;
    }
    m->seq = ld32(hdr + 4);
    m->op  = ld32(hdr + 8);
    uint32_t declared = ld16(hdr + 12);
    uint32_t padded = (declared + 15u) & ~15u;
    if (padded > sizeof m->payload) padded = (uint32_t)sizeof m->payload;   /* never overrun our buffer */
    uint32_t got = padded ? ring_read(c, m->payload, padded) : 0;
    if (got < padded) {
        hi.st.truncated++;                                /* delivered short, not dropped -- see comment above */
        memset(m->payload + got, 0, padded - got);
    }
    m->len = (uint16_t)(got < declared ? got : declared);
    wr16(hi.base + c->desc + D_RX_RD, c->rx_rd);          /* one publish, same place/timing as pre-v5 */
    bcm_io_unlock();
    hi.st.rx_msgs++;
    return 1;
}

/* ---------------------------------------------------------------- gencmd ------ */
int bcm_gencmd(const char *cmd, char *resp, size_t resp_sz, int max_polls)
{
    struct hi_chan *c = chan_by_type(CH_GENCMD);
    if (!hi.attached || !c) return -1;
    c->seq = (c->seq + 1) & 0x7fffffffu;
    hi.gc_wait = true; hi.gc_done = false; hi.gc_seq = c->seq;
    hi.gc_resp = resp; hi.gc_resp_sz = resp_sz;
    if (resp && resp_sz) resp[0] = 0;
    if (hi_tx(c, GENCMD_OPCODE, c->seq, cmd, (uint32_t)strlen(cmd) + 1) < 0) { hi.gc_wait = false; return -1; }
    for (int i = 0; i < max_polls && !hi.gc_done; i++) {
        bcm_host_service();
        if (!hi.gc_done) bcm_io_idle();
    }
    hi.gc_wait = false;
    return hi.gc_done ? hi.gc_result : -2;
}

static void gencmd_rx(const struct hi_msg *m)
{
    if (!hi.gc_wait || m->seq != hi.gc_seq) { TRACE("gencmd unsolicited", m->op, m->len, (const char *)m->payload); return; }
    if (hi.gc_resp && hi.gc_resp_sz) {
        size_t n = m->len < hi.gc_resp_sz - 1 ? m->len : hi.gc_resp_sz - 1;
        memcpy(hi.gc_resp, m->payload, n);
        hi.gc_resp[n] = 0;
    }
    hi.gc_result = (int)m->op;                            /* header result word */
    hi.gc_done = true;
}

/* ------------------------------------------------------------------ VCFS ------ */
void bcm_vfs_set_root(const char *root)
{
    size_t n = strlen(root);
    if (n >= sizeof hi.vfs_root) n = sizeof hi.vfs_root - 1;
    memcpy(hi.vfs_root, root, n); hi.vfs_root[n] = 0;
}

static void reply(struct hi_chan *c, const struct hi_msg *req, uint32_t result,
                  const void *payload, uint32_t len)
{
    hi_tx(c, result, req->seq, payload, len);   /* seq echoed as received, opcode slot = result */
}

static void vcfs_dispatch(struct hi_chan *c, const struct hi_msg *m)
{
    uint32_t p[4]; memcpy(p, m->payload, 16);
    const uint8_t *data = m->payload + 16;
    static uint8_t out[16 + HI_MAX_PAYLOAD];
    hi.st.vcfs_ops++;

    switch (m->op) {
    case VCFS_OPEN: {                                     /* stock: rt 0x287dd4 */
        /* Seen on real hardware (dlopen of "mplayer"): mode 1, path "\\Resources\\VideoCore\\Library\\mplayer"
         * - backslash separators and NO ".vll" extension - and then a fallback "\\mplayer". */
        uint32_t mode = p[0], h = 0xffffffffu;
        char path[192], req[128];
        size_t rl = 0, maxl = m->len > 16 ? m->len - 16u : 0;
        if (maxl > sizeof req - 1) maxl = sizeof req - 1;
        while (rl < maxl && data[rl]) { req[rl] = data[rl] == '\\' ? '/' : (char)data[rl]; rl++; }
        req[rl] = 0;
        if (mode != 0x20 && mode != 0x40) {               /* trunc / noreplace -> -1 in stock */
            static const char pre[] = "/Resources";
            bool ok = rl >= sizeof pre - 1 && !memcmp(req, pre, sizeof pre - 1);
            for (size_t i = 0; ok && i + 1 < rl; i++)               /* no path traversal */
                if (req[i] == '.' && req[i + 1] == '.') ok = false;
            size_t rootl = strlen(hi.vfs_root);
            if (ok && rootl + rl + 5 < sizeof path) {
                memcpy(path, hi.vfs_root, rootl);
                memcpy(path + rootl, req, rl);
                path[rootl + rl] = 0;
                int fd = open(path, O_RDONLY);
                if (fd < 0) {                             /* extension-less name: try <name>.vll */
                    bool has_dot = false;
                    for (size_t i = rl; i > 0 && req[i - 1] != '/'; i--)
                        if (req[i - 1] == '.') has_dot = true;
                    if (!has_dot) {
                        memcpy(path + rootl + rl, ".vll", 5);
                        fd = open(path, O_RDONLY);
                    }
                }
                if (fd >= 0) {
                    for (int i = 0; i < 8; i++)
                        if (hi.vfs_fd[i] < 0) { hi.vfs_fd[i] = fd; h = (uint32_t)i + 1; break; }
                    if (h == 0xffffffffu) close(fd);
                    else TRACE("vcfs mapped", 0, 0, path);
                }
            }
        }
        TRACE("vcfs open", mode, h, req);
        st32(out, h);
        reply(c, m, h == 0xffffffffu ? 1 : 0, out, 4);
        break; }
    case VCFS_CLOSE: {
        uint32_t h = p[0];
        if (h >= 1 && h <= 8 && hi.vfs_fd[h - 1] >= 0) { close(hi.vfs_fd[h - 1]); hi.vfs_fd[h - 1] = -1; }
        TRACE("vcfs close", h, 0, NULL);
        reply(c, m, 0, NULL, 0);
        break; }
    case VCFS_SEEK: {                                     /* whence 0/1/2, returns new position */
        uint32_t h = p[0]; int32_t off = (int32_t)p[1]; uint32_t wh = p[2];
        int64_t r = -1;
        if (h >= 1 && h <= 8 && hi.vfs_fd[h - 1] >= 0 && wh <= 2)
            r = lseek(hi.vfs_fd[h - 1], off, wh == 0 ? SEEK_SET : wh == 1 ? SEEK_CUR : SEEK_END);
        TRACE("vcfs seek", h, (uint32_t)(int32_t)r, NULL);
        st32(out, (uint32_t)(int32_t)r);
        reply(c, m, r < 0 ? 1 : 0, out, 4);
        break; }
    case VCFS_READ: {                                     /* fread(handle, size, count) */
        uint32_t h = p[0], size = p[1], count = p[2];
        int32_t got = -1;
        memset(out, 0, 16);
        if (h >= 1 && h <= 8 && hi.vfs_fd[h - 1] >= 0 && size) {
            uint32_t want = size * count;
            if (want > HI_MAX_PAYLOAD - 16) want = HI_MAX_PAYLOAD - 16;   /* real hardware asked for 0x800; a 0x800+16 reply was refused by the old 0x800 cap */
            got = (int32_t)read(hi.vfs_fd[h - 1], out + 16, want);
        }
        TRACE("vcfs read", h, (uint32_t)got, NULL);
        if (got < 0) { st32(out, 0xffffffffu); reply(c, m, 1, out, 4); break; }
        st32(out, (uint32_t)got / size);                  /* items read, in p0 */
        reply(c, m, 0, out, 16 + (uint32_t)got);          /* payload = 16 + bytes (rt 0xf9864) */
        break; }
    default:                                              /* stubs in stock return -1 -> error reply */
        hi.st.unknown_ops++;
        TRACE("vcfs unknown op", m->op, m->len, NULL);
        st32(out, 0xffffffffu);
        reply(c, m, 1, out, 4);
        break;
    }
}

/* ------------------------------------------------------------------- PDS ------ */
void bcm_pds_set_ops(const struct bcm_pds_ops *ops) { hi.pds_ops = ops; }
void bcm_pds_map_stream(int id, int buf) { if (id >= 0 && id < 4) hi.pds_stream_buf[id] = buf & 1; }

static int parse_dec(const char *s, size_t max)
{
    int v = 0, neg = 0; size_t i = 0;
    if (i < max && s[i] == '-') { neg = 1; i++; }
    for (; i < max && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

/* Announce/continue one chunk of a frame (rt 0x288c14): write data into the VC buffer, then
 * send the 12-word reply (header opcode 0x61, seq echoed from the last PDS request). */
static int pds_send_chunk(int b)
{
    struct hi_chan *c = chan_by_type(CH_VCPD);
    if (!c) return -1;
    if (!hi.pds_pend[b].active) return -1;
    struct bcm_pds_frame *f = &hi.pds_pend[b].f;
    uint32_t addr = hi.pds_buf[b].addr, cap = hi.pds_buf[b].cap;
    uint32_t remaining = f->length - hi.pds_pend[b].sent;
    uint32_t n = remaining < cap ? remaining : cap;
    bool first = (hi.pds_pend[b].sent == 0), last = (n == remaining);
    uint32_t flag = first ? (last ? PDS_FLAG_WHOLE : PDS_FLAG_FIRST)
                          : (last ? PDS_FLAG_LAST  : PDS_FLAG_MIDDLE);

    /* rt 0x287be8(dst=VC buffer, src, len rounded up to 16, port 0) */
    uint32_t padded = (n + 15u) & ~15u;
    static uint8_t bounce[4096 + 16];
    const uint8_t *src = f->data + hi.pds_pend[b].sent;
    bcm_io_lock();
    for (uint32_t off = 0; off < padded; off += sizeof bounce - 16) {
        uint32_t part = padded - off; if (part > sizeof bounce - 16) part = sizeof bounce - 16;
        uint32_t real = (off < n) ? (n - off < part ? n - off : part) : 0;
        memset(bounce, 0, part);
        if (real) memcpy(bounce, src + off, real);
        bcm_io_write(addr + off, bounce, part);
    }
    bcm_io_unlock();

    uint32_t w[12];
    w[0] = (uint32_t)(int32_t)f->stream_id;
    w[1] = (uint32_t)(int32_t)(int16_t)f->f2;
    w[2] = f->f4;
    w[3] = ++hi.pds_pend[b].counter;
    w[4] = 0;
    w[5] = n;
    w[6] = flag;
    w[7] = addr;
    memcpy(&w[8], f->meta, 16);
    if (hi_tx(c, PDS_FRAME_REPLY, hi.pds_last_seq, w, sizeof w) < 0) return -1;

    hi.pds_pend[b].sent += n;
    if (last) hi.pds_pend[b].active = false;
    return 0;
}

int bcm_pds_send_frame(const struct bcm_pds_frame *f)
{
    if (!hi.attached) return -1;
    int id = f->stream_id;
    int b = (id >= 0 && id < 4) ? hi.pds_stream_buf[id] : 0;
    if (hi.pds_pend[b].active || hi.pds_buf[b].cap == 0) return -1;
    hi.pds_pend[b].f = *f;
    hi.pds_pend[b].sent = 0;
    hi.pds_pend[b].active = true;
    return pds_send_chunk(b);
}

int bcm_pds_send_empty(void)
{
    struct hi_chan *c = chan_by_type(CH_VCPD);
    uint32_t w[2] = { 0, 0 };
    if (!hi.attached || !c) return -1;
    return hi_tx(c, PDS_FRAME_REPLY, hi.pds_last_seq, w, sizeof w);
}

static void pds_dispatch(struct hi_chan *c, const struct hi_msg *m)
{
    uint32_t w[6]; memset(w, 0, sizeof w);
    memcpy(w, m->payload, m->len < 24 ? m->len : 24);
    hi.st.pds_ops++;
    hi.pds_last_seq = m->seq;                             /* echoed in later replies */
    {   /* payload words as hex, for the log: the real values of the 0x41/0x42 fields are still unknown */
        static const char dig[] = "0123456789abcdef";
        char hx[64], *o = hx;
        for (int i = 0; i < 6; i++) {
            for (int k = 7; k >= 0; k--) *o++ = dig[(w[i] >> (4 * k)) & 15];
            *o++ = ' ';
        }
        *o = 0;
        TRACE("pds op", m->op, m->len, hx);
    }

    switch (m->op) {
    case PDS_PING:                                        /* only opcode with an immediate reply */
        reply(c, m, 0, m->payload, 4);
        break;
    case PDS_START: {
        hi.pds_buf[0].addr = w[2]; hi.pds_buf[0].cap = w[3];      /* buffer A */
        hi.pds_buf[1].addr = w[4]; hi.pds_buf[1].cap = w[5];      /* buffer B */
        memset(hi.pds_pend, 0, sizeof hi.pds_pend);
        int id = 0;
        if (m->len > 0x420) id = parse_dec((const char *)m->payload + 0x420, m->len - 0x420);
        hi.pds_cur_stream = id;
        if (hi.pds_ops && hi.pds_ops->start) hi.pds_ops->start(id, w[1]);
        break; }
    case PDS_GET_FRAME:
        /* continuation of a partially sent frame? (rt 0xff16c) */
        for (int b = 0; b < 2; b++)
            if (hi.pds_pend[b].active) { pds_send_chunk(b); return; }
        if (hi.pds_ops && hi.pds_ops->get_frame)
            hi.pds_ops->get_frame(hi.pds_cur_stream, w[1], w[2]);
        break;
    case PDS_SEEK:
        if (hi.pds_ops && hi.pds_ops->seek) hi.pds_ops->seek(hi.pds_cur_stream, w[1]);
        break;
    case PDS_STOP:
        memset(hi.pds_pend, 0, sizeof hi.pds_pend);
        if (hi.pds_ops && hi.pds_ops->stop) hi.pds_ops->stop();
        break;
    default:
        hi.st.unknown_ops++;                              /* stock: sets error, sends nothing */
        break;
    }
}

/* --------------------------------------------------------------- service ------ */
void bcm_host_service(void)
{
    if (!hi.attached) return;

    bcm_io_lock();
    uint32_t w = rd32(hi.base + H_TX_ACK);                /* +0x20 ack, +0x21 VC signal */
    uint8_t rx_sig = (uint8_t)(w >> 8);
    uint8_t pending = rx_sig ^ hi.rx_ack;
    if (pending) {                                        /* ack first, like the stock drain (rt 0x288100) */
        hi.rx_ack ^= pending;
        uint32_t h = rd32(hi.base + H_TX_SIG);
        h = (h & 0xffff0000u) | hi.tx_sig | ((uint32_t)hi.rx_ack << 8);
        wr32(hi.base + H_TX_SIG, h);
    }
    bcm_io_unlock();

    /* Drain every channel that signalled; the stock tasks loop until the ring is empty. */
    static struct hi_msg m;
    for (int i = 0; i < HI_NCHAN; i++) {
        struct hi_chan *c = &hi.ch[i];
        if (!c->present || !(pending & (1u << i))) continue;
        while (hi_rx_pop(c, &m)) {
            switch (c->type) {
            case CH_GENCMD: gencmd_rx(&m); break;
            case CH_VCFS:   vcfs_dispatch(c, &m); break;
            case CH_VCPD:   pds_dispatch(c, &m); break;
            case CH_VCHR:                                 /* hostreq: stock op 0x46 calls a stub returning 0 and replies */
                hi.st.unknown_ops++;
                TRACE("vchr op", m.op, m.len, NULL);
                if (m.op != 0x61) { uint32_t z = 0; reply(c, &m, 0, &z, 4); }
                break;
            case CH_GRAPHICS:
            default:        hi.st.unknown_ops++;
                            TRACE(c->type == CH_VCHR ? "vchr op" : "chan op", m.op, m.len, NULL);
                            break;
            }
        }
    }
}
