/* bcm overlay v74: docks / accessories -- notices, per-dock TV settings (apps/iap/iap-lingo0.c) */
#ifndef BCM_DOCK_H
#define BCM_DOCK_H
#include <stdbool.h>
#include "kernel.h"
#define SYS_IAP_DOCK  MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 6)   /* data: 1 connected, 0 gone, 2 power source */
#if defined(IPOD_VIDEO) && defined(IPOD_ACCESSORY_PROTOCOL)
void iap_dock_ui(void);                /* UI thread: notices, the dock's TV settings */
int iap_dock_tv_choice(void);          /* -1 unknown / no dock, 0 iPod screen, 1 TV */
void iap_dock_remember_tv(bool tv);
const char *iap_dock_name(void);       /* NULL = no dock */
#else
#define iap_dock_ui() do { } while (0)
#define iap_dock_tv_choice() (-1)
#define iap_dock_remember_tv(tv) do { } while (0)
#define iap_dock_name() ((const char *)0)
#endif
#endif
