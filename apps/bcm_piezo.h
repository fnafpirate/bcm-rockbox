/* bcm overlay v77: the click speaker (apps/bcm_piezo.c) */
#ifndef BCM_PIEZO_H
#define BCM_PIEZO_H
#if defined(IPOD_VIDEO) && !defined(SIMULATOR)
void bcm_piezo_apply(void);       /* settings -> firmware */
void bcm_piezo_startup(void);
void bcm_piezo_shutdown(void);
int bcm_piezo_test(void);
int bcm_piezo_test_music(void);   /* v78 */
#else
#define bcm_piezo_apply() do { } while (0)
#define bcm_piezo_startup() do { } while (0)
#define bcm_piezo_shutdown() do { } while (0)
#endif
#endif
