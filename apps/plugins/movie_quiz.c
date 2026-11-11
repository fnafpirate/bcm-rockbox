/***************************************************************************
 * bcm overlay v88: Movie Quiz -- guess the film from one random frame.
 *
 * The films come from the movies folder: a folder named Movies / Movie / Films (any case) at the top of the card or
 * inside a folder whose name has "video" in it (/Videos/Movies ...), the first with 2 or more videos; subfolders two
 * levels deep are searched too, for "Movies/Title (Year)/Title.mp4".  When there is none, every video in the video
 * folders is used, and then any video on the card (v92).
 *
 * A round: a random film is opened in the video player, muted and covered; it jumps to a random place between 10 % and
 * 85 % of its length (past the opening titles, before the credits), pauses there and the cover comes off.
 *   PLAY  = a hint: 3 more seconds (muted), then paused again
 *   MENU  = make your guess: four titles (the file names), one is right
 * The title is the file name without its extension, with "." and "_" as spaces.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later
 * version.
 ****************************************************************************/
#include "plugin.h"

#define MAX_FILMS   600
#define CHOICES     4
#define RECENT      8                    /* films not picked again until this many others came */

static char (*films)[MAX_PATH];          /* in the plugin buffer */
static int n_films, max_films;
static int recent[RECENT], n_recent;
static char titles[CHOICES][64];
static int choice_film[CHOICES];

static bool is_video(const char *name)
{
    const char *e = rb->strrchr(name, '.');
    return e && (!rb->strcasecmp(e, ".mp4") || !rb->strcasecmp(e, ".m4v") || !rb->strcasecmp(e, ".mov"));
}

/* v92: what the search saw, for the message when it finds too few */
static int seen_files, seen_other;
static bool counting;                    /* only in the video folders (not the music on the whole card) */
static char other_ext[8];

static void scan(const char *dir, int depth, int maxdepth)
{
    DIR *d = rb->opendir(dir);
    struct dirent *e;
    if (!d) return;
    while (n_films < max_films && (e = rb->readdir(d)) != NULL)
    {
        struct dirinfo inf = rb->dir_get_info(d, e);
        char p[MAX_PATH];
        if (e->d_name[0] == '.') continue;     /* ".", "..", hidden and macOS "._" files */
        if ((unsigned)rb->snprintf(p, sizeof p, "%s%s%s", dir, dir[1] ? "/" : "", e->d_name) >= sizeof p) continue;
        if (inf.attribute & ATTR_DIRECTORY)
        {
            if (depth < maxdepth) scan(p, depth + 1, maxdepth);
        }
        else if (is_video(e->d_name))
            /* v92: no size test (v88 read files over 2 GiB as size 0 and skipped every full-length film) */
            rb->strlcpy(films[n_films++], p, MAX_PATH);
        else if (counting)
        {
            const char *x = rb->strrchr(e->d_name, '.');
            seen_files++;
            if (x && rb->strcasecmp(x, ".srt") && rb->strcasecmp(x, ".jpg") && rb->strcasecmp(x, ".png")
                && rb->strcasecmp(x, ".txt") && rb->strcasecmp(x, ".nfo") && rb->strcasecmp(x, ".ini")
                && rb->strcasecmp(x, ".db"))
            {
                seen_other++;
                if (!other_ext[0]) rb->strlcpy(other_ext, x, sizeof other_ext);
            }
        }
    }
    rb->closedir(d);
}

static bool name_has(const char *name, const char *const *words)
{
    for (; *words; words++)
        if (rb->strcasestr(name, *words)) return true;
    return false;
}

/* v92: the movies folder is looked for by name, any case, one or two levels down: "Movies" / "Movie" / "Films" on
   its own or inside a folder whose name has "video" in it ("/Videos/Movies", "/iPod Videos/movies", "/Video's/Films"
   ...).  The first with 2 or more videos wins.  Then every video in every folder whose name has video / movie /
   film / tv in it, then the whole card (3 levels deep). */
static const char *find_films(void)
{
    static const char *const movie_words[] = { "movie", "film", NULL };
    static const char *const video_words[] = { "video", "movie", "film", "tv", NULL };
    static char where[MAX_PATH];
    char sub[MAX_PATH];
    DIR *root, *d2;
    struct dirent *e, *e2;
    int pass;

    for (pass = 0; pass < 2; pass++)
    {
        root = rb->opendir("/");
        if (!root) return NULL;
        while ((e = rb->readdir(root)) != NULL)
        {
            struct dirinfo inf = rb->dir_get_info(root, e);
            if (e->d_name[0] == '.' || !(inf.attribute & ATTR_DIRECTORY)) continue;
            rb->snprintf(where, sizeof where, "/%s", e->d_name);
            if (pass == 0 && name_has(e->d_name, movie_words))
            {   /* "/Movies" */
                n_films = 0;
                scan(where, 0, 2);
                if (n_films >= 2) { rb->closedir(root); return where; }
            }
            if (pass == 0 && name_has(e->d_name, video_words) && (d2 = rb->opendir(where)) != NULL)
            {   /* "/Videos/Movies" */
                while ((e2 = rb->readdir(d2)) != NULL)
                {
                    struct dirinfo i2 = rb->dir_get_info(d2, e2);
                    if (e2->d_name[0] == '.' || !(i2.attribute & ATTR_DIRECTORY) || !name_has(e2->d_name, movie_words))
                        continue;
                    rb->snprintf(sub, sizeof sub, "%s/%s", where, e2->d_name);
                    n_films = 0;
                    scan(sub, 0, 2);
                    if (n_films >= 2)
                    {
                        rb->closedir(d2); rb->closedir(root);
                        rb->strlcpy(where, sub, sizeof where);
                        return where;
                    }
                }
                rb->closedir(d2);
            }
            if (pass == 1 && name_has(e->d_name, video_words))
            {
                counting = true;
                scan(where, 0, 3);
                counting = false;
            }                 /* every video folder together */
        }
        rb->closedir(root);
        if (pass == 0) { n_films = 0; seen_files = seen_other = 0; other_ext[0] = 0; }
        else if (n_films >= 2) return "your video folders";
    }
    n_films = 0;
    scan("/", 0, 3);
    return n_films >= 2 ? "the card" : NULL;
}

/* "/Videos/Movies/The.Matrix_1999.mp4" -> "The Matrix 1999" */
static void title_of(const char *path, char *t, size_t n)
{
    const char *b = rb->strrchr(path, '/'), *e;
    size_t i = 0;
    b = b ? b + 1 : path;
    e = rb->strrchr(b, '.');
    if (!e || e == b) e = b + rb->strlen(b);
    for (; b < e && i + 1 < n; b++)
        t[i++] = (*b == '.' || *b == '_') ? ' ' : *b;
    t[i] = 0;
}

static bool was_recent(int f)
{
    int i;
    for (i = 0; i < n_recent; i++) if (recent[i] == f) return true;
    return false;
}

static int pick_film(void)
{
    int f, tries = 0;
    do f = rb->rand() % n_films; while (was_recent(f) && ++tries < 50);
    if (n_recent < RECENT && n_recent < n_films - 1) recent[n_recent++] = f;
    else if (n_recent)
    {
        int i;
        for (i = 1; i < n_recent; i++) recent[i - 1] = recent[i];
        recent[n_recent - 1] = f;
    }
    return f;
}

static const char *choice_name(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    return titles[item];
}

static int choice_action(int action, struct gui_synclist *lists)
{
    (void)lists;
    return action == ACTION_STD_OK ? ACTION_STD_CANCEL : action;   /* picked: leave the list with the selection set */
}

/* the four (or fewer) titles, the right one at a random place; returns how many */
static int make_choices(int right)
{
    int n = n_films < CHOICES ? n_films : CHOICES, i, k, tries;
    int at = rb->rand() % n;
    for (i = 0; i < n; i++)
    {
        int f = right;
        if (i != at)
            for (tries = 0; tries < 200; tries++)
            {
                bool dup = false;
                f = rb->rand() % n_films;
                if (f == right) continue;
                for (k = 0; k < i; k++) if (choice_film[k] == f) dup = true;
                if (!dup)
                {   /* two copies of one film (different files, same title) would make two right answers */
                    char a[64], b[64];
                    title_of(films[f], a, sizeof a); title_of(films[right], b, sizeof b);
                    if (rb->strcasecmp(a, b)) break;
                }
            }
        choice_film[i] = f;
        title_of(films[f], titles[i], sizeof titles[i]);
    }
    return n;
}

enum plugin_status plugin_start(const void *parameter)
{
    size_t bufsize;
    const char *where;
    int score = 0, rounds = 0;
    bool first = true;
    (void)parameter;

    films = rb->plugin_get_buffer(&bufsize);
    max_films = MIN(MAX_FILMS, (int)(bufsize / MAX_PATH));
    if (max_films < 2)
        return PLUGIN_ERROR;
    rb->srand(*rb->current_tick);

    rb->splash(0, "Looking for movies...");
    where = find_films();
    if (!where)
    {
        if (n_films == 1)
            rb->splashf(6 * HZ, "Only 1 video found (%s). Movie Quiz needs 2 or more .mp4 / .m4v / .mov files",
                        films[0]);
        else if (seen_other)
            rb->splashf(6 * HZ, "No .mp4 / .m4v / .mov videos found (%d other file(s), e.g. %s). "
                        "Movie Quiz needs 2 or more in /Videos/Movies", seen_other, other_ext);
        else
            rb->splash(6 * HZ, "No videos found. Movie Quiz needs 2 or more .mp4 / .m4v / .mov files in /Videos/Movies");
        return PLUGIN_OK;
    }
    rb->splashf(2 * HZ, "%d movies in %s.  PLAY = 3 s hint, MENU = guess", n_films, where);

    for (;;)
    {
        struct simplelist_info info;
        char title[48], answer[64];
        int right = pick_film(), n, permille = 100 + rb->rand() % 751;   /* 10.0 % .. 85.0 % */
        if (!rb->bcm_video_quiz(films[right], permille, first))
        {
            if (first) return PLUGIN_OK;     /* backed out of the TV / iPod question, or the player could not start */
            rb->splash(2 * HZ, "That one would not play -- next");
            continue;
        }
        first = false;
        n = make_choices(right);
        rb->snprintf(title, sizeof title, "Which movie?  (%d / %d)", score, rounds);
        rb->simplelist_info_init(&info, title, n, NULL);
        info.get_name = choice_name;
        info.action_callback = choice_action;
        info.selection = 0;
        info.scroll_all = true;
        rb->simplelist_show_list(&info);
        if (info.selection < 0 || info.selection >= n)
            break;                            /* MENU / back: the end */
        rounds++;
        title_of(films[right], answer, sizeof answer);
        if (choice_film[info.selection] == right)
        {
            score++;
            rb->splashf(2 * HZ, "Right!  %s   (%d / %d)", answer, score, rounds);
        }
        else
            rb->splashf(3 * HZ, "No -- it was %s   (%d / %d)", answer, score, rounds);
        if (!rb->yesno_pop("Next movie?"))
            break;
    }
    if (rounds)
        rb->splashf(3 * HZ, "Final score: %d of %d", score, rounds);
    return PLUGIN_OK;
}
