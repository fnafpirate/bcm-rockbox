/* bcm overlay v105: the iPod's clock set from the paired computer over USB (see bcm_timesync.c) */
#ifndef BCM_TIMESYNC_H
#define BCM_TIMESYNC_H

struct ts_record
{
    int year, mon, day, hour, min, sec, ms;   /* the computer's local time when it wrote the record */
    unsigned long utc;                        /* the same moment in seconds since 1970 (UTC) */
    char pc[64];                              /* computer name, shown on screen */
    char id[64];                              /* computer id (Windows MachineGuid): what pairing compares */
    char seq[24];                             /* random per record: the same record is never applied twice */
};

/* parses a "RBTSYNC1" record; 0 = ok, < 0 = not usable */
int ts_parse(const char *rec, struct ts_record *r);

/* called by the USB screen after the cable comes out: applies the record written during the connection, if any */
void bcm_timesync_after_usb(void);

#endif
