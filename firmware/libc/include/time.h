/*
 * time.h
 * 
 * Struct declaration for dealing with time.
 */

#ifndef _TIME_H_
#define _TIME_H_

#ifdef WPSEDITOR
#include "inttypes.h"
#include <time.h>
#endif

struct tm
{
  int   tm_sec;
  int   tm_min;
  int   tm_hour;
  int   tm_mday;
  int   tm_mon;
  int   tm_year;
  int   tm_wday;
  int   tm_yday;
  int   tm_isdst;
};

#if !defined(_TIME_T_DEFINED) && !defined(_TIME_T_DECLARED)
/* bcm overlay v74: the Year-2038 problem.  A signed 32-bit time_t ends at 2038-01-19 03:14:07 and then jumps to
   1901: file dates (browser, dircache, "file changed" checks), the playback log's timestamps and plugins such as
   the TOTP authenticator would all go wrong.  Nothing here ever needs a time before 1970 (the RTC counts
   2000-2099, FAT / exFAT dates start in 1980), so time_t is unsigned: same size (no structure or plugin-interface
   change), good until 2106-02-07 06:28:15 -- past the RTC's own 2099 and FAT's 2107. */
typedef unsigned long time_t;

/* this define below is used by the mingw headers to prevent duplicate
   typedefs */
#define _TIME_T_DEFINED
#define _TIME_T_DECLARED
time_t time(time_t *t);
struct tm *localtime(const time_t *timep);
struct tm *gmtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
time_t mktime(struct tm *t);

#endif /* SIMULATOR */

#ifdef __PCTOOL__
/* this time.h does not define struct timespec,
   so tell sys/stat.h not to use it */
#undef __USE_MISC  
#endif

#endif /* _TIME_H_ */


