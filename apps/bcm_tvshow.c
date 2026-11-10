/***************************************************************************
 * bcm overlay v74: TV series -- which episode comes next.
 *
 * A video is an episode when its path has a folder named "TV Show" / "TV Shows" / "TVShows" / "TV" (any case, with
 * a space, "_", "-" or "." in between) and the show's own folder below it:
 *     /Video's/TV Show/Some Show/S01E03 Title.mp4
 *     /Videos/TV Shows/Some Show/Season 2/05 - Title.m4v
 * The episodes of the show are all its videos (its folder and one level of subfolders, "Season 1" ...), ordered by
 * season and episode read from the names:  S01E03, s1e3, 1x03, "Season 2"/"Series 2"/"S2" folders with E05, "Episode 5",
 * "Ep 5", "Ep. 5", "Part 5", or a leading number ("05 - Title", "105 Title" = season 1 episode 5 when the folder gives
 * no season).  Names without numbers keep their natural order ("Title 2" before "Title 10").
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later
 * version.
 ****************************************************************************/
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#ifdef TVS_HOST_TEST
#include <dirent.h>
#include <strings.h>
#define MAX_PATH 260
#else
#include "config.h"
#include "dir.h"
#include "string-extra.h"
#include "core_alloc.h"
#endif
#include "bcm_tvshow.h"

static bool is_video_name(const char *n)
{
    const char *e = strrchr(n, '.');
    return e && (!strcasecmp(e, ".mp4") || !strcasecmp(e, ".m4v") || !strcasecmp(e, ".mov"));
}
/* "TV Show", "tv_shows", "TVShows", "TV-Show", "TV" */
static bool is_tv_folder(const char *s, size_t n)
{
    char b[16];
    size_t i, o = 0;
    if (n >= sizeof b + 8) return false;
    for (i = 0; i < n && o < sizeof b - 1; i++)
        if (s[i] != ' ' && s[i] != '_' && s[i] != '-' && s[i] != '.') b[o++] = (char)tolower((unsigned char)s[i]);
    b[o] = 0;
    return !strcmp(b, "tvshow") || !strcmp(b, "tvshows") || !strcmp(b, "tv") || !strcmp(b, "tvseries");
}
bool tvs_show_root(const char *path, char *root, size_t n, char *show, size_t sn)
{
    const char *p = path, *seg, *end;
    while (*p == '/') p++;
    while (*p)
    {
        seg = p;
        end = strchr(p, '/');
        if (!end) return false;                   /* the file itself */
        if (is_tv_folder(seg, (size_t)(end - seg)))
        {
            const char *s2 = end + 1, *e2 = strchr(s2, '/');
            size_t l;
            if (!e2 || e2 == s2) return false;    /* the video must be inside a show's folder */
            l = (size_t)(e2 - path);
            if (l + 1 > n) return false;
            memcpy(root, path, l); root[l] = 0;
            if (show && sn)
            {
                size_t k = (size_t)(e2 - s2) < sn - 1 ? (size_t)(e2 - s2) : sn - 1;
                memcpy(show, s2, k); show[k] = 0;
            }
            return true;
        }
        p = end + 1;
    }
    return false;
}

/* ---- names -> season / episode */
static const char *find_ci(const char *h, const char *needle)
{
    size_t l = strlen(needle);
    for (; *h; h++) if (!strncasecmp(h, needle, l)) return h;
    return NULL;
}
static int num_at(const char *p, int maxd, const char **after)
{
    int v = 0, d = 0;
    while (d < maxd && isdigit((unsigned char)p[d])) { v = v * 10 + (p[d] - '0'); d++; }
    if (after) *after = p + d;
    return d ? v : -1;
}
/* a season from a folder name: "Season 2", "Series 02", "S2", "Staffel 3", "Saison 4", "2" */
static int folder_season(const char *f)
{
    static const char *const w[] = { "season", "series", "staffel", "saison", "temporada", "stagione" };
    unsigned i;
    const char *p;
    for (i = 0; i < sizeof w / sizeof w[0]; i++)
        if ((p = find_ci(f, w[i])) != NULL)
        {
            p += strlen(w[i]);
            while (*p == ' ' || *p == '_' || *p == '.' || *p == '-') p++;
            return num_at(p, 3, NULL);
        }
    if ((f[0] == 's' || f[0] == 'S') && isdigit((unsigned char)f[1])) return num_at(f + 1, 3, NULL);
    if (isdigit((unsigned char)f[0])) { const char *a; int v = num_at(f, 3, &a); if (!*a) return v; }
    return -1;
}
void tvs_parse(const char *name, int folder_s, int *season, int *episode)
{
    const char *p;
    int s = -1, e = -1;
    /* S01E02, s1e2, S01.E02, S01 E02 */
    for (p = name; *p; p++)
        if ((*p == 's' || *p == 'S') && isdigit((unsigned char)p[1]) && (p == name || !isalnum((unsigned char)p[-1])))
        {
            const char *a; int sv = num_at(p + 1, 3, &a);
            while (*a == '.' || *a == ' ' || *a == '_' || *a == '-') a++;
            if ((*a == 'e' || *a == 'E') && isdigit((unsigned char)a[1])) { s = sv; e = num_at(a + 1, 4, NULL); break; }
        }
    /* 1x02 */
    if (e < 0)
        for (p = name; *p; p++)
            if (isdigit((unsigned char)*p) && (p == name || !isalnum((unsigned char)p[-1])))
            {
                const char *a; int sv = num_at(p, 2, &a);
                if ((*a == 'x' || *a == 'X') && isdigit((unsigned char)a[1]) && sv >= 0)
                {
                    const char *b; int ev = num_at(a + 1, 3, &b);
                    if (!isdigit((unsigned char)*b)) { s = sv; e = ev; break; }
                }
            }
    /* Episode 5, Ep 5, Ep. 5, E05, Part 5 */
    if (e < 0)
    {
        static const char *const w[] = { "episode", "ep.", "ep", "part", "pt.", "folge", "e" };
        unsigned i;
        for (i = 0; i < sizeof w / sizeof w[0] && e < 0; i++)
            for (p = name; (p = find_ci(p, w[i])) != NULL; p++)
            {
                const char *a = p + strlen(w[i]);
                if (p != name && isalnum((unsigned char)p[-1])) continue;
                while (*a == ' ' || *a == '_' || *a == '.' || *a == '-') a++;
                if (isdigit((unsigned char)*a)) { e = num_at(a, 4, NULL); break; }
            }
    }
    /* a leading number: "05 - Title", "105 Title" (3 digits, no season from the folder: 1 + 05) */
    if (e < 0 && isdigit((unsigned char)name[0]))
    {
        const char *a; int v = num_at(name, 4, &a);
        if (!isdigit((unsigned char)*a) && *a != 'p' && *a != 'P')   /* not "1080p" */
        {
            if (folder_s < 0 && a - name == 3 && v >= 101) { s = v / 100; e = v % 100; }
            else if (a - name < 4) e = v;
        }
    }
    if (s < 0) s = folder_s;
    *season = s; *episode = e;
}

/* natural compare: digits by value */
int tvs_natcmp(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b))
        {
            const char *x = a, *y = b; long va, vb;
            va = strtol(a, (char **)&x, 10); vb = strtol(b, (char **)&y, 10);
            if (va != vb) return va < vb ? -1 : 1;
            a = x; b = y;
            continue;
        }
        {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca < cb ? -1 : 1;
        }
        a++; b++;
    }
    return *a ? 1 : *b ? -1 : 0;
}

struct ep { int s, e; char rel[MAX_PATH]; };
static int ep_cmp(const void *x, const void *y)
{
    const struct ep *a = x, *b = y;
    int sa = a->s < 0 ? 0 : a->s, sb = b->s < 0 ? 0 : b->s;
    if (sa != sb) return sa < sb ? -1 : 1;
    if (a->e >= 0 && b->e >= 0 && a->e != b->e) return a->e < b->e ? -1 : 1;
    return tvs_natcmp(a->rel, b->rel);
}
static int scan_dir(const char *root, const char *sub, struct ep *t, int n, int max, int depth)
{
    char path[MAX_PATH];
    DIR *d;
    struct dirent *de;
    int fs = sub[0] ? folder_season(sub) : -1;
    snprintf(path, sizeof path, "%s%s%s", root, sub[0] ? "/" : "", sub);
    d = opendir(path);
    if (!d) return n;
    while ((de = readdir(d)) != NULL && n < max)
    {
        const char *nm = (const char *)de->d_name;
        char full[MAX_PATH];
        bool dir;
        if (nm[0] == '.') continue;
        snprintf(full, sizeof full, "%s/%s", path, nm);
#ifdef TVS_HOST_TEST
        dir = de->d_type == DT_DIR;
#else
        dir = (dir_get_info(d, de).attribute & ATTR_DIRECTORY) != 0;
#endif
        if (dir)
        {
            if (depth == 0 && !sub[0])
            {
                char s2[MAX_PATH];
                snprintf(s2, sizeof s2, "%s", nm);
                closedir(d);                       /* one level down, then back (keeps one DIR open at a time) */
                n = scan_dir(root, s2, t, n, max, 1);
                d = opendir(path);
                if (!d) return n;
                /* skip forward to after this folder */
                while ((de = readdir(d)) != NULL && strcmp((const char *)de->d_name, s2)) ;
                if (!de) break;
            }
            continue;
        }
        if (!is_video_name(nm)) continue;
        snprintf(t[n].rel, sizeof t[n].rel, "%s%s%s", sub, sub[0] ? "/" : "", nm);
        tvs_parse(nm, fs, &t[n].s, &t[n].e);
        n++;
    }
    if (d) closedir(d);
    return n;
}
int tvs_next(const char *cur, int dir, char *out, size_t n, char *label, size_t ln)
{
    char root[MAX_PATH], show[64];
    struct ep *t;
    int cnt, i, found = -1, max = 1000, ret = 0;
#ifndef TVS_HOST_TEST
    int h;
#endif
    if (!tvs_show_root(cur, root, sizeof root, show, sizeof show)) return 0;
#ifdef TVS_HOST_TEST
    t = malloc(sizeof *t * max);
#else
    h = core_alloc_ex(sizeof *t * max, NULL);
    if (h <= 0) return 0;
    core_pin(h);
    t = core_get_data(h);
#endif
    if (!t) return 0;
    cnt = scan_dir(root, "", t, 0, max, 0);
    qsort(t, cnt, sizeof *t, ep_cmp);
    {
        size_t rl = strlen(root);
        const char *rel = cur + rl + 1;
        for (i = 0; i < cnt; i++) if (!strcmp(t[i].rel, rel)) { found = i; break; }
    }
    i = found + dir;
    if (found >= 0 && i >= 0 && i < cnt)
    {
        const char *b = strrchr(t[i].rel, '/'), *e;
        snprintf(out, n, "%s/%s", root, t[i].rel);
        b = b ? b + 1 : t[i].rel;
        e = strrchr(b, '.');
        if (label && ln)
        {
            int k = (int)(e ? (size_t)(e - b) : strlen(b));
            if (t[i].s >= 0 && t[i].e >= 0)
                snprintf(label, ln, "%s  S%02dE%02d", show, t[i].s, t[i].e);
            else
                snprintf(label, ln, "%s", show);
            if (strlen(label) + 3 < ln)
                snprintf(label + strlen(label), ln - strlen(label), "\n%.*s", k, b);
        }
        ret = 1;
    }
#ifdef TVS_HOST_TEST
    free(t);
#else
    core_unpin(h); core_free(h);
#endif
    return ret;
}
