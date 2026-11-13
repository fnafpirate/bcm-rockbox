/***************************************************************************
 * bcm overlay v105: clock sync over USB, from one paired computer.
 *
 * USB storage gives the iPod no way to ask the computer for the time, so a small helper on the computer
 * (tools/bcm/ipod_time_sync.pyw, started with Windows) writes /.rockbox/timesync.dat when the iPod's drive appears:
 * one 512-byte sector
 *
 *     RBTSYNC1
 *     local=2026-11-12 20:05:31.123
 *     utc=1794531931
 *     pc=DESKTOP-PAUL
 *     id=<Windows MachineGuid>
 *     seq=<random>
 *
 * firmware/usbstack/usb_storage.c catches that sector as it is written and notes the tick.  After the cable comes
 * out, the record is applied here: local time + the time since it arrived -> the RTC.
 *
 * Pairing: the first computer that sends a record is remembered in /.rockbox/timesync_pc.txt.  A record from any
 * other computer is ignored (and says so).  Delete that file to pair another computer.
 ***************************************************************************/
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "bcm_timesync.h"

#ifndef TS_HOST_TEST
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "timefuncs.h"
#include "splash.h"
#include "lang.h"
#include "time.h"
#endif

#define TS_PAIR_FILE "/.rockbox/timesync_pc.txt"

/* value of "key=" in a key=value text (lines end in \n); copied printable-only into out; false = missing */
static bool ts_value(const char *text, const char *key, char *out, int n)
{
    int kl = strlen(key);
    const char *p = text;
    while (p && *p)
    {
        if (!strncmp(p, key, kl) && p[kl] == '=')
        {
            int i = 0;
            p += kl + 1;
            while (*p && *p != '\n' && *p != '\r' && i < n - 1)
            {
                if (*p >= 0x20 && *p < 0x7f) out[i++] = *p;
                p++;
            }
            out[i] = 0;
            while (i > 0 && out[i - 1] == ' ') out[--i] = 0;
            return i > 0;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return false;
}

int ts_parse(const char *rec, struct ts_record *r)
{
    char v[64];
    memset(r, 0, sizeof *r);
    if (strncmp(rec, "RBTSYNC1", 8)) return -1;
    if (!ts_value(rec, "local", v, sizeof v)) return -2;
    if (sscanf(v, "%d-%d-%d %d:%d:%d.%d", &r->year, &r->mon, &r->day, &r->hour, &r->min, &r->sec, &r->ms) < 6)
        return -3;
    if (r->year < 2000 || r->year > 2099 || r->mon < 1 || r->mon > 12 || r->day < 1 || r->day > 31 ||
        r->hour < 0 || r->hour > 23 || r->min < 0 || r->min > 59 || r->sec < 0 || r->sec > 59 ||
        r->ms < 0 || r->ms > 999)
        return -4;
    if (ts_value(rec, "utc", v, sizeof v)) r->utc = strtoul(v, NULL, 10);
    if (!ts_value(rec, "id", r->id, sizeof r->id)) return -5;
    if (!ts_value(rec, "pc", r->pc, sizeof r->pc)) strcpy(r->pc, "?");
    ts_value(rec, "seq", r->seq, sizeof r->seq);
    return 0;
}

#ifndef TS_HOST_TEST
extern bool usb_storage_timesync_take(char *rec, int n, long *tick);

void bcm_timesync_after_usb(void)
{
    static char rec[512], pair[512];
    char pid[64] = "", plast[24] = "", pseq[24] = "";
    struct ts_record r;
    struct tm tm;
    long tick, dt_ms;
    time_t t;
    int fd, n;
    bool paired;

    if (!usb_storage_timesync_take(rec, sizeof rec, &tick))
        return;                                       /* no helper on this computer, or it wrote nothing */
    if (ts_parse(rec, &r) != 0)
        return;

    fd = open(TS_PAIR_FILE, O_RDONLY);
    paired = fd >= 0;
    if (paired)
    {
        n = read(fd, pair, sizeof pair - 1);
        close(fd);
        pair[n > 0 ? n : 0] = 0;
        ts_value(pair, "id", pid, sizeof pid);
        ts_value(pair, "last", plast, sizeof plast);
        ts_value(pair, "seq", pseq, sizeof pseq);
        if (pid[0] && strcmp(pid, r.id))
        {
            splashf(HZ * 2, str(LANG_TIMESYNC_NOT_PAIRED), r.pc);
            return;
        }
        if (r.seq[0] && !strcmp(r.seq, pseq))
            return;                                   /* the same record again (e.g. a restored backup) */
        if (r.utc && plast[0] && r.utc + 3600 < strtoul(plast, NULL, 10))
            return;                                   /* older than the last one applied: an old copy of the file */
    }

    memset(&tm, 0, sizeof tm);
    tm.tm_year = r.year - 1900; tm.tm_mon = r.mon - 1; tm.tm_mday = r.day;
    tm.tm_hour = r.hour; tm.tm_min = r.min; tm.tm_sec = r.sec;
    t = mktime(&tm);
    dt_ms = (current_tick - tick) * (1000 / HZ) + r.ms;  /* written that long ago (the cable may have stayed in) */
    if (dt_ms < 0) dt_ms = 0;
    t += (dt_ms + 500) / 1000;
    gmtime_r(&t, &tm);                                /* mktime / gmtime_r here: no time zone, local in = local out */
    if (set_time(&tm) != 0)
        return;

    fd = open(TS_PAIR_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        fdprintf(fd, "# iPod clock sync: the computer this iPod takes its time from over USB.\n"
                     "# Delete this file to pair another computer (the next one that syncs).\n"
                     "id=%s\npc=%s\nlast=%lu\nseq=%s\n", r.id, r.pc, r.utc, r.seq);
        close(fd);
    }
    splashf(HZ * 2, str(LANG_TIMESYNC_SET), r.pc);
}
#endif
