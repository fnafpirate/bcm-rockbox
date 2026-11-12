/* bcm overlay v74: TV series episode order (apps/bcm_tvshow.c) */
#ifndef BCM_TVSHOW_H
#define BCM_TVSHOW_H
#include <stdbool.h>
#include <stddef.h>
/* the show's folder ("/Video's/TV Show/Some Show") of an episode path, and the show's name; false = not an episode */
bool tvs_show_root(const char *path, char *root, size_t n, char *show, size_t sn);
/* season / episode from a file name (-1 = none); folder_season = from its folder ("Season 2") or -1 */
void tvs_parse(const char *name, int folder_season, int *season, int *episode);
int tvs_natcmp(const char *a, const char *b);
/* the next (dir +1) or previous (-1) episode of the show: its path in out, and "Show  S01E04\nTitle" in label.
   Returns 1 if there is one. */
int tvs_next(const char *cur, int dir, char *out, size_t n, char *label, size_t ln);
#endif
