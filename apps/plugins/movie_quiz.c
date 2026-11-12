/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/     \/            \/
 * $Id$
 *
 * Movie Quiz plugin — Plugins > Games.
 * Presents multiple-choice questions about video files found in
 * /.rockbox/movie_quiz/ or the current video directory.
 *
 * Copyright (C) 2024 bcm-rockbox contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "plugin.h"

/* Minimum plugin API version required */
#if (PLUGIN_MIN_API_VERSION < 287)
#error "movie_quiz requires plugin API 287 or later"
#endif

/* -----------------------------------------------------------------------
 * Quiz data types
 * --------------------------------------------------------------------- */
#define MAX_TITLES   256
#define MAX_CHOICES  4

typedef struct {
    char title[64];
} movie_title_t;

typedef struct {
    char           question[128];
    char           choices[MAX_CHOICES][64];
    int            correct;   /* 0-3 */
} quiz_question_t;

/* -----------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------- */
static movie_title_t s_titles[MAX_TITLES];
static int           s_ntitles;

static quiz_question_t s_question;
static int             s_score;
static int             s_total;

/* -----------------------------------------------------------------------
 * Scan for video files
 * --------------------------------------------------------------------- */
static void scan_titles(const char *dir)
{
    s_ntitles = 0;
    DIR *dp = rb->opendir(dir);
    if (!dp) return;

    struct dirent *de;
    while ((de = rb->readdir(dp)) != NULL && s_ntitles < MAX_TITLES)
    {
        const char *dot = rb->strrchr(de->d_name, '.');
        if (!dot) continue;
        if (rb->strcasecmp(dot, ".mp4") == 0 ||
            rb->strcasecmp(dot, ".m4v") == 0 ||
            rb->strcasecmp(dot, ".mov") == 0)
        {
            /* Strip extension for the display title */
            size_t nlen = (size_t)(dot - de->d_name);
            if (nlen >= sizeof(s_titles[0].title))
                nlen = sizeof(s_titles[0].title) - 1;
            rb->memcpy(s_titles[s_ntitles].title, de->d_name, nlen);
            s_titles[s_ntitles].title[nlen] = '\0';
            s_ntitles++;
        }
    }
    rb->closedir(dp);
}

/* -----------------------------------------------------------------------
 * Generate a "What is the name of this movie?" question.
 * --------------------------------------------------------------------- */
static bool gen_question(void)
{
    if (s_ntitles < MAX_CHOICES) return false;

    /* Pick a random correct answer */
    int correct_idx = rb->rand() % s_ntitles;
    s_question.correct = rb->rand() % MAX_CHOICES;

    rb->snprintf(s_question.question, sizeof(s_question.question),
                 "Which of these is a video file you have?");

    /* Fill choices: put the correct title at s_question.correct,
     * fill others with random different titles. */
    bool used[MAX_TITLES];
    rb->memset(used, 0, sizeof(used));
    used[correct_idx] = true;

    rb->strlcpy(s_question.choices[s_question.correct],
                s_titles[correct_idx].title,
                sizeof(s_question.choices[0]));

    for (int slot = 0; slot < MAX_CHOICES; slot++)
    {
        if (slot == s_question.correct) continue;
        int pick;
        do {
            pick = rb->rand() % s_ntitles;
        } while (used[pick]);
        used[pick] = true;
        rb->strlcpy(s_question.choices[slot],
                    s_titles[pick].title,
                    sizeof(s_question.choices[0]));
    }
    return true;
}

/* -----------------------------------------------------------------------
 * Display and input
 * --------------------------------------------------------------------- */
static int show_question(void)
{
    int selected = 0;
    bool confirmed = false;

    while (!confirmed)
    {
        rb->lcd_clear_display();
        rb->lcd_puts(0, 0, "Movie Quiz");
        rb->lcd_puts(0, 1, s_question.question);
        rb->lcd_puts(0, 2, "");

        for (int i = 0; i < MAX_CHOICES; i++)
        {
            char line[72];
            rb->snprintf(line, sizeof(line), "%c %s",
                         (i == selected) ? '>' : ' ',
                         s_question.choices[i]);
            rb->lcd_puts(0, 3 + i, line);
        }

        char score_line[32];
        rb->snprintf(score_line, sizeof(score_line),
                     "Score: %d/%d", s_score, s_total);
        rb->lcd_puts(0, 7, score_line);
        rb->lcd_update();

        int btn = rb->button_get(true);
        if (btn == BUTTON_UP)    { if (selected > 0) selected--; }
        if (btn == BUTTON_DOWN)  { if (selected < MAX_CHOICES-1) selected++; }
        if (btn == BUTTON_SELECT || btn == BUTTON_RIGHT)
            confirmed = true;
        if (btn == BUTTON_LEFT || btn == ACTION_STD_CANCEL)
            return -1;  /* quit */
    }
    return selected;
}

/* -----------------------------------------------------------------------
 * Plugin entry point
 * --------------------------------------------------------------------- */
enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;

    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "Movie Quiz");
    rb->lcd_puts(0, 1, "Scanning video files...");
    rb->lcd_update();

    /* Try the quiz data dir first, then root */
    scan_titles("/.rockbox/movie_quiz");
    if (s_ntitles < MAX_CHOICES)
        scan_titles("/");

    if (s_ntitles < MAX_CHOICES)
    {
        rb->lcd_clear_display();
        rb->lcd_puts(0, 0, "Need at least 4 video files.");
        rb->lcd_puts(0, 1, "Put .mp4 files in");
        rb->lcd_puts(0, 2, "/.rockbox/movie_quiz/");
        rb->lcd_update();
        rb->button_get(true);
        return PLUGIN_OK;
    }

    s_score = 0;
    s_total = 0;

    for (int round = 0; round < 10; round++)
    {
        if (!gen_question()) break;

        int chosen = show_question();
        if (chosen < 0) break;

        s_total++;
        bool correct = (chosen == s_question.correct);
        if (correct) s_score++;

        rb->lcd_clear_display();
        rb->lcd_puts(0, 0, correct ? "Correct!" : "Wrong!");
        rb->lcd_puts(0, 1, "Answer:");
        rb->lcd_puts(0, 2, s_question.choices[s_question.correct]);
        char score_line[32];
        rb->snprintf(score_line, sizeof(score_line),
                     "Score: %d/%d", s_score, s_total);
        rb->lcd_puts(0, 4, score_line);
        rb->lcd_update();
        rb->sleep(HZ * 2);
    }

    /* Final score */
    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "Game Over!");
    char final[32];
    rb->snprintf(final, sizeof(final), "Final: %d/%d", s_score, s_total);
    rb->lcd_puts(0, 1, final);
    rb->lcd_update();
    rb->button_get(true);

    return PLUGIN_OK;
}
