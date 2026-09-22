/*
 * bcm_host_test.c - host-side tests for bcm_host.c against a mock VideoCore.
 *
 * The mock implements the ring/signal semantics exactly as decoded from Firmware-25.6.3
 * (see bcm_host.c header). It proves the module is self-consistent with those semantics.
 * It does NOT prove the semantics match real hardware: that needs the device.
 *
 *   gcc -std=gnu11 -Wall -Wextra -DBCM_HOST_TEST -I.. -o bcm_host_test bcm_host_test.c ../bcm_host.c
 *   ./bcm_host_test <path-to-a-real-file-to-serve, e.g. passthruhandler.vll>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "bcm_host.h"
#include "bcm_host_io.h"

/* ------------------------------------------------------------- mock VC RAM ---- */
static uint8_t vcram[1 << 22];
#define BASE 0x200u
static uint32_t RING = 0x1000;           /* bytes per ring (a PDS 0x41 message alone is ~0x430 B), overridden by the wrap test */
static int doorbells;
static void (*idle_hook)(void);
static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

void bcm_io_lock(void) {}
void bcm_io_unlock(void) {}
void bcm_io_doorbell(void) { doorbells++; }
void bcm_io_idle(void) { if (idle_hook) idle_hook(); }
void bcm_io_read(uint32_t a, void *d, size_t n)  { if ((a | n) & 3) { printf("BAD unaligned read %x/%zu\n", a, n); exit(2); }
    if ((uint64_t)a + n > sizeof vcram) { memset(d, 0xFF, n); return; }   /* unmapped VC address reads as ones */
    memcpy(d, vcram + a, n); }
void bcm_io_write(uint32_t a, const void *s, size_t n) { if ((a | n) & 3) { printf("BAD unaligned write %x/%zu\n", a, n); exit(2); }
    if ((uint64_t)a + n <= sizeof vcram) memcpy(vcram + a, s, n); }

static uint16_t g16(uint32_t a) { uint16_t v; memcpy(&v, vcram + a, 2); return v; }
static void     s16(uint32_t a, uint16_t v) { memcpy(vcram + a, &v, 2); }
static void     s32(uint32_t a, uint32_t v) { memcpy(vcram + a, &v, 4); }

/* ---- channel layout: directory idx 0..4 = types 1,2,5,6,7 ---- */
static const uint16_t types[5] = { 1, 2, 5, 6, 7 };
#define D(i)        (BASE + 0x40 + (i) * 0x50)                      /* descriptor abs addr */
#define TXS(i)      (0x400u + (i) * 2 * 0x1000u)                    /* ring offsets, relative to BASE */
#define RXS(i)      (TXS(i) + 0x1000u)

static void vc_init(void)
{
    memset(vcram, 0, sizeof vcram);
    s32(0x1F0, 0xA5A50002u); s32(0x1F8, 1); s32(0x1FC, BASE);
    for (int i = 0; i < 5; i++) {
        s16(BASE + i * 2, (uint16_t)(D(i) - BASE));
        uint32_t d = D(i);
        s16(d + 0x04, types[i]);
        s16(d + 0x06, TXS(i));       s16(d + 0x08, TXS(i) + RING);
        s16(d + 0x0a, RXS(i));       s16(d + 0x0c, RXS(i) + RING);
        s16(d + 0x10, TXS(i));       s16(d + 0x20, TXS(i));         /* TX rd / wr */
        s16(d + 0x30, RXS(i));       s16(d + 0x40, RXS(i));         /* RX rd / wr */
    }
    doorbells = 0;
}

/* ---- mock VC sending to host (RX ring), semantics of rt 0x28871c mirrored ---- */
static uint32_t used(uint16_t st, uint16_t en, uint16_t rd, uint16_t wr)
{ return wr < rd ? (uint32_t)(en - rd) + (wr - st) : (uint32_t)(wr - rd); }

static void vc_ring_put(int ch, const uint8_t *src, uint32_t len)
{
    uint32_t d = D(ch);
    uint16_t st = g16(d + 0x0a), en = g16(d + 0x0c), rd = g16(d + 0x30), wr = g16(d + 0x40);
    while (len) {
        uint16_t lim = (st == rd) ? en : rd;
        int32_t max = (int32_t)lim - 0x10;
        uint32_t n;
        if ((int32_t)wr > max) { n = en - wr; if (n > len) n = len; }
        else                   { n = max - wr; if (n > len) n = len; }
        if (!n) { printf("mock VC RX ring full\n"); exit(2); }
        memcpy(vcram + BASE + wr, src, n);
        wr += n; src += n; len -= n;
        if (wr == en) wr = st;
        s16(d + 0x40, wr);
    }
}
static void vc_send(int ch, uint32_t op, uint32_t seq, const void *payload, uint16_t len)
{
    uint8_t hdr[16] = {0}; uint32_t magic = 0xF1A55A1Fu;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &seq, 4); memcpy(hdr + 8, &op, 4); memcpy(hdr + 12, &len, 2);
    uint32_t padded = (len + 15u) & ~15u;
    uint8_t *pl = calloc(1, padded ? padded : 1);
    if (len) memcpy(pl, payload, len);
    vc_ring_put(ch, hdr, 16);
    if (padded) vc_ring_put(ch, pl, padded);
    free(pl);
    uint8_t bit = 1u << ch, sig = vcram[BASE + 0x21], ack = vcram[BASE + 0x11];
    vcram[BASE + 0x21] = sig ^ (bit & ~(sig ^ ack));          /* toggle unless pending */
}

/* ---- mock VC receiving from host (TX ring) into a per-channel byte queue ---- */
static uint8_t  q[5][1 << 16];
static uint32_t qn[5];

static void vc_drain(int ch)
{
    uint32_t d = D(ch);
    uint16_t st = g16(d + 0x06), en = g16(d + 0x08), rd = g16(d + 0x10), wr = g16(d + 0x20);
    uint32_t n = used(st, en, rd, wr), got = 0;
    while (got < n) {
        uint32_t chunk = (wr < rd || 1) ? (en - rd) : 0;
        if (rd < wr) chunk = wr - rd;
        if (chunk > n - got) chunk = n - got;
        memcpy(q[ch] + qn[ch], vcram + BASE + rd, chunk);
        qn[ch] += chunk; got += chunk; rd += chunk;
        if (rd == en) rd = st;
    }
    s16(d + 0x10, rd);
    vcram[BASE + 0x20] = vcram[BASE + 0x10];                  /* ack every pending host signal */
}

struct rmsg { uint32_t seq, op; uint16_t len; uint8_t payload[4096]; };
static int vc_recv(int ch, struct rmsg *m)
{
    vc_drain(ch);
    if (qn[ch] < 16) return 0;
    uint32_t magic; memcpy(&magic, q[ch], 4);
    CHECK(magic == 0xF1A55A1Fu, "reply magic %08x", magic);
    memcpy(&m->seq, q[ch] + 4, 4); memcpy(&m->op, q[ch] + 8, 4); memcpy(&m->len, q[ch] + 12, 2);
    uint32_t padded = (m->len + 15u) & ~15u;
    if (qn[ch] < 16 + padded) return 0;
    memcpy(m->payload, q[ch] + 16, padded);
    memmove(q[ch], q[ch] + 16 + padded, qn[ch] - 16 - padded);
    qn[ch] -= 16 + padded;
    return 1;
}
static void q_reset(void) { memset(qn, 0, sizeof qn); }

/* -------------------------------------------------------------- test bodies ---- */
static void drain_idle(void);
static void gencmd_idle(void)                     /* the mock "VideoCore" answers a gencmd */
{
    struct rmsg m;
    if (vc_recv(0, &m)) {
        char resp[128];
        if (!strcmp((char *)m.payload, "version")) snprintf(resp, sizeof resp, "version=Mar 10 2008 15:17:43");
        else snprintf(resp, sizeof resp, "error=1 error_msg=\"bad argument\"");
        CHECK(m.op == 1, "gencmd opcode %u", m.op);
        CHECK((m.seq & 0x80000000u) == 0, "host request seq has bit31 clear");
        vc_send(0, 0, m.seq, resp, (uint16_t)(strlen(resp) + 1));
    }
}

static void test_gencmd(void)
{
    char resp[128];
    idle_hook = gencmd_idle;
    int r = bcm_gencmd("version", resp, sizeof resp, 100);
    CHECK(r == 0, "gencmd result %d", r);
    CHECK(!strcmp(resp, "version=Mar 10 2008 15:17:43"), "gencmd resp '%s'", resp);
    r = bcm_gencmd("nonsense", resp, sizeof resp, 100);
    CHECK(strstr(resp, "bad argument") != NULL, "second gencmd '%s'", resp);
    idle_hook = NULL;
    CHECK(doorbells >= 2, "doorbell rung");
}

static uint32_t vcfs_call(uint32_t op, uint32_t p0, uint32_t p1, uint32_t p2, const void *data, uint16_t dlen,
                          struct rmsg *out)
{
    static uint32_t seq = 0x80000100u;
    uint8_t pl[16 + 512] = {0};
    memcpy(pl, &p0, 4); memcpy(pl + 4, &p1, 4); memcpy(pl + 8, &p2, 4);
    if (dlen) memcpy(pl + 16, data, dlen);
    vc_send(2, op, ++seq, pl, (uint16_t)(16 + dlen));
    bcm_host_service();
    int ok = vc_recv(2, out);
    CHECK(ok, "no VCFS reply for op %02x", op);
    CHECK(out->seq == seq, "VCFS reply seq %08x != %08x", out->seq, seq);
    return ok ? out->op : 0xdeadbeef;                              /* result word */
}

static int trace_opens, trace_reads;
static void tracer(const char *tag, uint32_t a, uint32_t b, const char *s)
{ (void)a; (void)b; (void)s; if (!strcmp(tag, "vcfs open")) trace_opens++; if (!strcmp(tag, "vcfs read")) trace_reads++; }

static void test_vcfs(const char *file)
{
    bcm_host_set_trace(tracer);
    struct rmsg r; uint32_t res;
    struct stat sb; stat(file, &sb);
    idle_hook = drain_idle;                       /* the mock VC consumes the TX ring while the host waits */

    /* build root/Resources/VideoCore/Library/<name> pointing at the test file */
    char root[] = "/tmp/vcroot_XXXXXX"; if (!mkdtemp(root)) { perror("mkdtemp"); exit(2); }
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p %s/Resources/VideoCore/Library && cp '%s' %s/Resources/VideoCore/Library/test.vll", root, file, root);
    if (system(cmd)) exit(2);
    bcm_vfs_set_root(root);

    const char *good = "/Resources/VideoCore/Library/test.vll";
    res = vcfs_call(0x4c, 8, 0, 0, good, (uint16_t)strlen(good) + 1, &r);
    uint32_t h; memcpy(&h, r.payload, 4);
    CHECK(res == 0 && h != 0 && h != 0xffffffffu, "open ok res=%u h=%u", res, h);

    /* as seen on real hardware: backslashes and no extension (dlopen of "test") */
    const char *hw = "\\Resources\\VideoCore\\Library\\test";
    res = vcfs_call(0x4c, 1, 0, 0, hw, (uint16_t)strlen(hw) + 1, &r);
    uint32_t h2; memcpy(&h2, r.payload, 4);
    CHECK(res == 0 && h2 != 0 && h2 != 0xffffffffu, "backslash + extension-less path opens test.vll (res=%u)", res);
    vcfs_call(0x41, h2, 0, 0, NULL, 0, &r);
    const char *nosuch = "\\Resources\\VideoCore\\Library\\nosuchmodule";
    res = vcfs_call(0x4c, 1, 0, 0, nosuch, (uint16_t)strlen(nosuch) + 1, &r);
    CHECK(res == 1, "missing module -> error reply");
    const char *rel = "\\test";
    res = vcfs_call(0x4c, 1, 0, 0, rel, (uint16_t)strlen(rel) + 1, &r);
    CHECK(res == 1, "the VC's fallback probe of a bare name is refused");
    const char *bsdots = "\\Resources\\..\\etc\\passwd";
    res = vcfs_call(0x4c, 1, 0, 0, bsdots, (uint16_t)strlen(bsdots) + 1, &r);
    CHECK(res == 1, "backslash path traversal rejected");

    const char *bad = "/Resources/../etc/passwd";
    res = vcfs_call(0x4c, 8, 0, 0, bad, (uint16_t)strlen(bad) + 1, &r);
    CHECK(res == 1, "path traversal rejected (res=%u)", res);
    const char *outside = "/etc/passwd";
    res = vcfs_call(0x4c, 8, 0, 0, outside, (uint16_t)strlen(outside) + 1, &r);
    CHECK(res == 1, "path outside /Resources rejected");
    res = vcfs_call(0x4c, 0x20, 0, 0, good, (uint16_t)strlen(good) + 1, &r);
    CHECK(res == 1, "trunc mode (0x20) rejected like stock");

    res = vcfs_call(0x43, h, 0, 2, NULL, 0, &r);                   /* seek(0, END) */
    uint32_t pos; memcpy(&pos, r.payload, 4);
    CHECK(res == 0 && pos == (uint32_t)sb.st_size, "size via seek END = %u (want %ld)", pos, (long)sb.st_size);
    res = vcfs_call(0x43, h, 0, 0, NULL, 0, &r);
    memcpy(&pos, r.payload, 4);
    CHECK(res == 0 && pos == 0, "seek SET 0");

    /* read whole file in 1-byte-item reads of up to 1000 items, compare with the original */
    uint8_t *got = malloc(sb.st_size + 4096); size_t total = 0;
    for (;;) {
        res = vcfs_call(0x44, h, 1, 1000, NULL, 0, &r);
        uint32_t count; memcpy(&count, r.payload, 4);
        CHECK(res == 0, "read res %u", res);
        CHECK(r.len == 16 + count, "read payload len %u == 16 + %u", r.len, count);
        if (!count) break;
        memcpy(got + total, r.payload + 16, count); total += count;
        if (total > (size_t)sb.st_size + 1) break;
    }
    FILE *f = fopen(file, "rb"); uint8_t *ref = malloc(sb.st_size);
    if (fread(ref, 1, sb.st_size, f) != (size_t)sb.st_size) exit(2);
    fclose(f);
    CHECK(total == (size_t)sb.st_size && !memcmp(got, ref, sb.st_size), "file content round-trips through VCFS (%zu bytes)", total);
    free(got); free(ref);

    /* exactly what real hardware asked for: fread(handle, 1, 0x800) - a 0x810-byte reply */
    res = vcfs_call(0x43, h, 0, 0, NULL, 0, &r);
    static uint8_t big[16 + 0x4010];
    { uint32_t p[4] = { h, 1, 0x800, 0 }; uint8_t pl[16]; memcpy(pl, p, 16);
      vc_send(2, 0x44, 0x80000900u, pl, 16); bcm_host_service();
      struct rmsg *rr = (struct rmsg *)big; (void)rr; }
    { static struct rmsg rb; int ok = vc_recv(2, &rb);
      uint32_t cnt = 0; if (ok) memcpy(&cnt, rb.payload, 4);
      CHECK(ok && rb.op == 0 && rb.len == 16 + cnt && cnt == (sb.st_size >= 0x800 ? 0x800u : (uint32_t)sb.st_size),
            "0x800-byte read (as sent by the VC) is answered: ok=%d len=%u count=%u", ok, ok ? rb.len : 0, cnt); }

    res = vcfs_call(0x41, h, 0, 0, NULL, 0, &r);
    CHECK(res == 0 && r.len == 0, "close");
    CHECK(trace_opens == 8 && trace_reads > 3, "trace hook saw opens=%d reads=%d", trace_opens, trace_reads);
    res = vcfs_call(0x44, h, 1, 10, NULL, 0, &r);
    CHECK(res == 1, "read after close is an error");
    res = vcfs_call(0x42, 0, 0, 0, NULL, 0, &r);
    CHECK(res == 1, "stub opcode 0x42 -> error, like stock returning -1");
}

/* PDS ---------------------------------------------------------------------- */
static struct { int start_id; uint32_t start_w1; int getf; uint32_t gf_w1, gf_w2; int seek_id; uint32_t seek_pos; int stops; } pd;
static void on_start(int id, uint32_t w1) { pd.start_id = id; pd.start_w1 = w1; }
static void on_getf(int id, uint32_t w1, uint32_t w2) { (void)id; pd.getf++; pd.gf_w1 = w1; pd.gf_w2 = w2; }
static void on_seek(int id, uint32_t pos) { pd.seek_id = id; pd.seek_pos = pos; }
static void on_stop(void) { pd.stops++; }

static void pds_req(uint32_t op, uint32_t seq, uint32_t w1, uint32_t w2, uint32_t w3, uint32_t w4, uint32_t w5,
                    const char *str)
{
    uint8_t pl[0x440] = {0}; uint32_t w[6] = {0, w1, w2, w3, w4, w5};
    memcpy(pl, w, 24);
    uint16_t len = 24;
    if (str) { strcpy((char *)pl + 0x420, str); len = 0x420 + (uint16_t)strlen(str) + 1; }
    vc_send(4, op, seq, pl, len);
    bcm_host_service();
}

static void expect_frame_reply(uint32_t seq, uint32_t nbytes, uint32_t flag, uint32_t addr, const uint8_t *expect,
                               uint32_t stream, const uint32_t meta[4])
{
    struct rmsg r; int ok = vc_recv(4, &r);
    CHECK(ok, "PDS frame reply present");
    if (!ok) return;
    uint32_t w[12]; memcpy(w, r.payload, 48);
    CHECK(r.op == 0x61, "reply opcode %02x", r.op);
    CHECK(r.seq == seq, "reply echoes last request seq (%08x vs %08x)", r.seq, seq);
    CHECK(r.len == 48, "reply payload 12 words");
    CHECK(w[0] == stream && w[5] == nbytes && w[6] == flag && w[7] == addr,
          "words: id=%u len=%u flag=%u addr=%x (want %u %u %u %x)", w[0], w[5], w[6], w[7], stream, nbytes, flag, addr);
    CHECK(!memcmp(&w[8], meta, 16), "metadata words copied");
    CHECK(!memcmp(vcram + addr, expect, nbytes), "chunk data present in VC buffer");
    for (uint32_t i = nbytes; i < ((nbytes + 15) & ~15u); i++) CHECK(vcram[addr + i] == 0, "padding zero at +%u", i);
}

static void test_pds(void)
{
    static const struct bcm_pds_ops ops = { on_start, on_getf, on_seek, on_stop };
    idle_hook = drain_idle;
    bcm_pds_set_ops(&ops);

    pds_req(0x40, 0x80000001u, 0xabcd0123u, 0, 0, 0, 0, NULL);     /* ping */
    { struct rmsg r; CHECK(vc_recv(4, &r) && r.op == 0 && r.len == 4, "ping reply");
      uint32_t e; memcpy(&e, r.payload, 4); CHECK(e == 0, "ping echoes payload word 0 (=0 here)"); }

    const uint32_t A = 0x100000, B = 0x200000;
    pds_req(0x41, 0x80000002u, 7, A, 4096, B, 2048, "3");
    CHECK(pd.start_id == 3 && pd.start_w1 == 7, "start: id=%d w1=%u", pd.start_id, pd.start_w1);

    pds_req(0x42, 0x80000003u, 11, 22, 0, 0, 0, NULL);
    CHECK(pd.getf == 1 && pd.gf_w1 == 11 && pd.gf_w2 == 22, "get_frame callback");

    /* a 10000-byte frame goes into buffer A (stream 0) in 4096/4096/1808 chunks */
    uint8_t *frame = malloc(10000);
    for (int i = 0; i < 10000; i++) frame[i] = (uint8_t)(i * 7 + 3);
    struct bcm_pds_frame f = { .stream_id = 0, .f2 = 5, .f4 = 1234, .length = 10000,
                               .meta = { 0x11, 0x22, 0x33, 0x44 }, .data = frame };
    CHECK(bcm_pds_send_frame(&f) == 0, "send_frame");
    expect_frame_reply(0x80000003u, 4096, PDS_FLAG_FIRST, A, frame, 0, f.meta);
    CHECK(bcm_pds_send_frame(&f) < 0, "second frame on the same buffer refused while one is in flight");

    pds_req(0x42, 0x80000004u, 0, 0, 0, 0, 0, NULL);               /* continuation */
    expect_frame_reply(0x80000004u, 4096, PDS_FLAG_MIDDLE, A, frame + 4096, 0, f.meta);
    CHECK(pd.getf == 1, "continuation did not call the app");
    pds_req(0x42, 0x80000005u, 0, 0, 0, 0, 0, NULL);
    expect_frame_reply(0x80000005u, 1808, PDS_FLAG_LAST, A, frame + 8192, 0, f.meta);

    /* a small frame on stream 1 -> buffer B, whole-frame flag, 100 bytes padded to 112 */
    memset(vcram + B, 0xEE, 256);
    struct bcm_pds_frame g = { .stream_id = 1, .f2 = 0, .f4 = 99, .length = 100, .meta = {1, 2, 3, 4}, .data = frame };
    CHECK(bcm_pds_send_frame(&g) == 0, "small frame");
    expect_frame_reply(0x80000005u, 100, PDS_FLAG_WHOLE, B, frame, 1, g.meta);

    /* "no frame" answer: 8-byte payload of zeros, opcode 0x61, seq of the last request */
    CHECK(bcm_pds_send_empty() == 0, "send_empty");
    { struct rmsg r; CHECK(vc_recv(4, &r) && r.op == 0x61 && r.len == 8 && r.seq == 0x80000005u, "empty reply framing");
      uint32_t z[2]; memcpy(z, r.payload, 8); CHECK(z[0] == 0 && z[1] == 0, "empty reply payload zero"); }

    pds_req(0x43, 0x80000006u, 4321, 0, 0, 0, 0, NULL);
    CHECK(pd.seek_id == 3 && pd.seek_pos == 4321, "seek callback");
    pds_req(0x44, 0x80000007u, 0, 0, 0, 0, 0, NULL);
    CHECK(pd.stops == 1, "stop callback");
    { struct rmsg r; CHECK(!vc_recv(4, &r), "opcodes 0x41-0x44 send no immediate reply"); }
    pds_req(0x55, 0x80000008u, 0, 0, 0, 0, 0, NULL);
    { struct rmsg r; CHECK(!vc_recv(4, &r), "unknown opcode: no reply (stock prepares an error but skips the send)"); }
    free(frame);
}

/* Ring wrap and back-pressure with tiny rings -------------------------------------------- */
static void drain_idle(void) { for (int i = 0; i < 5; i++) vc_drain(i); }

static void test_wrap(const char *file)
{
    RING = 0x100;                                  /* 256-byte rings: every message wraps sooner or later */
    vc_init(); q_reset();
    CHECK(bcm_host_attach() == 0, "re-attach with small rings");
    char root[] = "/tmp/vcroot_XXXXXX"; if (!mkdtemp(root)) exit(2);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p %s/Resources && cp '%s' %s/Resources/w.bin", root, file, root);
    if (system(cmd)) exit(2);
    bcm_vfs_set_root(root);
    idle_hook = drain_idle;

    struct rmsg r; uint32_t res;
    res = vcfs_call(0x4c, 8, 0, 0, "/Resources/w.bin", 17, &r);
    uint32_t h; memcpy(&h, r.payload, 4);
    CHECK(res == 0, "open in tiny-ring mode");
    int bad = 0;
    for (int i = 0; i < 600; i++) {                /* 600 seeks: header+16 B each way, wraps ~ every 8 msgs */
        res = vcfs_call(0x43, h, (uint32_t)(i % 50), 0, NULL, 0, &r);
        uint32_t pos; memcpy(&pos, r.payload, 4);
        if (res != 0 || pos != (uint32_t)(i % 50)) bad++;
    }
    CHECK(bad == 0, "600 request/reply round trips across ring wraps (%d bad)", bad);

    /* a read reply of 16+N bytes larger than the ring capacity forces hi_tx to wait for the VC */
    res = vcfs_call(0x43, h, 0, 0, NULL, 0, &r);
    res = vcfs_call(0x44, h, 1, 1000, NULL, 0, &r);
    uint32_t count; memcpy(&count, r.payload, 4);
    CHECK(res == 0 && count > 240, "large read reply through a 256-byte ring (%u bytes)", count);
    FILE *f = fopen(file, "rb"); uint8_t ref[2048]; size_t n = fread(ref, 1, count, f); fclose(f);
    CHECK(n == count && !memcmp(ref, r.payload + 16, count), "large reply content intact");
    idle_hook = NULL;
}

/* A VC->host message larger than the RX ring, delivered while the host is already reading it
   (real hardware: a 500-byte `commands` reply through a 0x200-byte ring). */
static uint32_t vc_ring_put_some(int ch, const uint8_t *src, uint32_t len)
{
    uint32_t d = D(ch), done = 0;
    uint16_t st = g16(d + 0x0a), en = g16(d + 0x0c), rd = g16(d + 0x30), wr = g16(d + 0x40);
    while (done < len) {
        uint16_t lim = (st == rd) ? en : rd;
        int32_t max = (int32_t)lim - 0x10, n;
        if ((int32_t)wr > max) n = en - wr; else n = max - wr;
        if (n <= 0) break;
        if ((uint32_t)n > len - done) n = (int32_t)(len - done);
        memcpy(vcram + BASE + wr, src + done, n);
        wr += n; done += n; if (wr == en) wr = st;
        s16(d + 0x40, wr);
    }
    return done;
}
/* Real hardware evidence (session 3, run 5): a VC reply that does not fit the ring in one write
 * (a ~540-byte `commands` list through a 512-byte ring) is delivered SHORT -- the VC does not seem
 * to top it up -- and the header still declares the full length. hi_rx_pop must treat that as a
 * truncated-but-valid message, not wait for more (waiting hung for 5s and then desynced every
 * later reply on the channel: bad_magic=5 in that log). */
static void test_truncated_reply(void)
{
    RING = 0x200; vc_init(); q_reset();
    CHECK(bcm_host_attach() == 0, "attach with 0x200 rings");
    char text[700]; for (int i = 0; i < 699; i++) text[i] = (char)('a' + i % 26); text[699] = 0;
    uint8_t hdr[16] = {0}; uint32_t magic = 0xF1A55A1Fu, seq = 0x80000001u, op = 2; uint16_t len = 700;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &seq, 4); memcpy(hdr + 8, &op, 4); memcpy(hdr + 12, &len, 2);
    /* write only what fits in one pass (240-ish usable bytes for this ring), exactly what a
       non-retrying VC producer would do -- no idle_hook draining in between */
    uint32_t wrote = vc_ring_put_some(0, hdr, 16);
    wrote += vc_ring_put_some(0, (const uint8_t *)text, 700 - (wrote > 16 ? 0 : 0));
    (void)wrote;
    vcram[BASE + 0x21] ^= 1;

    char resp[1024];
    int r = bcm_gencmd("commands", resp, sizeof resp, 50);
    CHECK(r == -2, "the FIRST poll only sees the truncated message: bcm_gencmd's own request was never sent"
                    " in this test (we injected the reply directly), so it correctly times out (r=%d) --"
                    " what matters is what bcm_host_service() delivered, checked next", r);
    const struct bcm_host_stats *st = bcm_host_get_stats();
    CHECK(st->truncated >= 1, "truncated counter incremented (%u)", st->truncated);
    CHECK(st->bad_magic == 0, "no bad_magic -- a short reply is not a protocol error (%u)", st->bad_magic);
    RING = 0x1000; vc_init(); q_reset();
}

static void gencmd_idle_truncate_commands(void)
{
    struct rmsg m;
    if (!vc_recv(0, &m)) return;
    if (!strcmp((char *)m.payload, "commands")) {
        /* the VC's own reply, but only the part one ring pass can carry -- no closing quote,
           exactly the missing-quote truncation observed on real hardware */
        char text[700]; for (int i = 0; i < 699; i++) text[i] = (char)('a' + i % 26); text[699] = 0;
        uint8_t hdr[16] = {0}; uint32_t magic = 0xF1A55A1Fu, op = 2; uint16_t len = 700;
        memcpy(hdr, &magic, 4); memcpy(hdr + 4, &m.seq, 4); memcpy(hdr + 8, &op, 4); memcpy(hdr + 12, &len, 2);
        vc_ring_put_some(0, hdr, 16);
        vc_ring_put_some(0, (const uint8_t *)text, 700);       /* only what fits actually lands */
        vcram[BASE + 0x21] ^= 1;
    } else {
        char resp[32] = "ok";
        vc_send(0, 0, m.seq, resp, 3);
    }
}
static void test_gencmd_after_truncation(void)
{
    /* the real regression: does the channel keep working for the NEXT command? */
    RING = 0x200; vc_init(); q_reset();
    CHECK(bcm_host_attach() == 0, "attach with 0x200 rings");
    idle_hook = gencmd_idle_truncate_commands;
    char resp[1024];
    int r1 = bcm_gencmd("version", resp, sizeof resp, 200);
    CHECK(r1 == 0, "version still works first (%d)", r1);
    int r2 = bcm_gencmd("commands", resp, sizeof resp, 200);
    CHECK(r2 == 2 && strlen(resp) < 700, "commands returns the truncated text rather than hanging (r=%d len=%zu)", r2, strlen(resp));
    int r3 = bcm_gencmd("tasks", resp, sizeof resp, 200);
    CHECK(r3 == 0, "tasks still works right after (%d) -- the channel was not desynced", r3);
    idle_hook = NULL; RING = 0x1000; vc_init(); q_reset();
}

static void test_vchr_reply(void)
{
    vc_init(); q_reset();
    CHECK(bcm_host_attach() == 0, "attach");
    uint8_t pl[16] = {0};
    vc_send(3, 0x46, 0x80000abcu, pl, 4);                          /* type 6 = VCHR is directory index 3 */
    bcm_host_service();
    struct rmsg r;
    CHECK(vc_recv(3, &r) && r.op == 0 && r.len == 4 && r.seq == 0x80000abcu, "VCHR op 0x46 is answered with result 0 and a 4-byte payload");
    uint32_t z = 1; memcpy(&z, r.payload, 4); CHECK(z == 0, "payload is 0");
}

/* attach() must not depend on VC[0x1F8]==1, and must reject a nonsense info pointer ------------ */
static void test_attach_variants(void)
{
    /* what real hardware showed under Rockbox: VC[0x1F8] = the legacy LCD command 0xFFFF0000 */
    vc_init(); q_reset();
    s32(0x1F8, 0xFFFF0000u);
    CHECK(bcm_host_attach() == 0, "attach succeeds although VC[0x1F8] != 1");
    const struct bcm_host_diag *dg = bcm_host_get_diag();
    CHECK(!dg->ready_ok && dg->w1f8 == 0xFFFF0000u && dg->w1fc == BASE, "diag records the words (ready_ok=%d)", dg->ready_ok);
    CHECK(dg->type[0] == 1 && dg->type[2] == 5 && dg->type[4] == 7, "diag types");

    /* the value seen on real hardware in VC[0x1FC]: an address outside anything mapped here */
    vc_init(); s32(0x1F8, 0xFFFF0000u); s32(0x1FC, 0xFE000CA0u);
    CHECK(bcm_host_attach() == -4, "garbage directory rejected (-4)");
    dg = bcm_host_get_diag();
    CHECK(dg->w1fc == 0xFE000CA0u, "diag still records VC[0x1FC]");

    vc_init(); s32(0x1FC, 0);
    CHECK(bcm_host_attach() == -2, "zero info pointer rejected (-2)");
    vc_init(); s32(0x1FC, BASE + 2);
    CHECK(bcm_host_attach() == -2, "unaligned info pointer rejected (-2)");
    vc_init();
}

/* real hardware directory: 0030 0680 48d0 4bb0 5740 - offsets well above 0x4000, types in an order
   other than 1,2,5,6,7 (index 1 was type 5) */
static void test_high_directory(void)
{
    vc_init(); q_reset();
    uint32_t hi_off[5] = { 0x30, 0x680, 0x48d0, 0x4bb0, 0x5740 };
    uint8_t save[5][0x50];
    for (int i = 0; i < 5; i++) memcpy(save[i], vcram + D(i), 0x50);
    for (int i = 0; i < 5; i++) { memcpy(vcram + BASE + hi_off[i], save[i], 0x50); s16(BASE + i * 2, (uint16_t)hi_off[i]); }
    s16(BASE + 2, 0x680); memcpy(vcram + BASE + 0x680 + 4, &(uint16_t){5}, 2);          /* index 1 = type 5 */
    CHECK(bcm_host_attach() == 0, "attach with descriptors at high offsets");
    const struct bcm_host_diag *dg = bcm_host_get_diag();
    CHECK(dg->dir[4] == 0x5740 && dg->type[4] == 7 && dg->type[1] == 5, "all five channels seen (type[4]=%u type[1]=%u)", dg->type[4], dg->type[1]);
    CHECK(dg->tx_end[4] > dg->tx_start[4] && dg->rx_end[4] > dg->rx_start[4], "ring bounds recorded");
    vc_init();
}

int main(int argc, char **argv)
{
    const char *file = argc > 1 ? argv[1] : "/mnt/user-data/uploads/passthruhandler.vll";
    vc_init();
    CHECK(bcm_host_attach() == 0, "attach");
    printf("[gencmd]\n"); fflush(stdout); test_gencmd();
    printf("[vcfs]\n"); fflush(stdout); test_vcfs(file);
    printf("[pds]\n"); fflush(stdout); test_pds();
    printf("[ring wrap / back-pressure]\n"); fflush(stdout); test_wrap(file);
    printf("[attach variants]\n"); fflush(stdout); test_attach_variants();
    printf("[high directory offsets]\n"); fflush(stdout); test_high_directory();
    printf("[truncated reply]\n"); fflush(stdout); test_truncated_reply();
    printf("[gencmd survives truncation]\n"); fflush(stdout); test_gencmd_after_truncation();
    printf("[vchr]\n"); fflush(stdout); test_vchr_reply();
    const struct bcm_host_stats *st = bcm_host_get_stats();
    printf("stats: rx=%u tx=%u vcfs=%u pds=%u bad_magic=%u unknown=%u\n",
           st->rx_msgs, st->tx_msgs, st->vcfs_ops, st->pds_ops, st->bad_magic, st->unknown_ops);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
