/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Björn Stenberg
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

#include <stdio.h>
#include <stdbool.h>
#include "action.h"
#include "font.h"
#ifdef HAVE_REMOTE_LCD
#include "lcd-remote.h"
#endif
#include "lang.h"
#include "usb.h"
#if defined(HAVE_USBSTACK)
#include "usb_core.h"
#ifdef USB_ENABLE_HID
#include "usb_keymaps.h"
#endif
#endif
#include "settings.h"
#include "led.h"
#include "appevents.h"
#include "usb_screen.h"
#include "splash.h"
#include "file.h"
#include "viewport.h"
#include "skin_engine/skin_engine.h"
#include "playlist.h"
#include "misc.h"
#include "icons.h"
#if defined(IPOD_VIDEO) && !defined(SIMULATOR)
/* bcm overlay v76: live USB statistics under the USB logo */
#include "usb_drv.h"
#include "ata.h"
#include "powermgmt.h"
#include "power.h"
#include "system.h"
#define USB_STATS 1
extern volatile unsigned long long usb_stat_read_bytes, usb_stat_write_bytes;   /* usbstack/usb_storage.c */
#endif

#include "bitmaps/usblogo.h"

#ifdef HAVE_REMOTE_LCD
#include "bitmaps/remote_usblogo.h"
#endif

#if (CONFIG_STORAGE & STORAGE_MMC)
#include "ata_mmc.h"
#endif

struct usb_screen_vps_t
{
    struct viewport parent;
    struct viewport logo;
#ifndef USB_ENABLE_HID
};
#else
    struct viewport title;
};

int usb_keypad_mode;
static void draw_usb_keypad_mode(struct viewport *title)
{
    struct screen *screen = &screens[SCREEN_MAIN];
    struct viewport *last_vp = screen->set_viewport(title);
    screen->clear_viewport();
    title->flags |= VP_FLAG_ALIGN_CENTER;
    if (title->width > 1)
        screen->puts_scroll(0, 0, str(keypad_mode_name_get(usb_keypad_mode)));
    screen->set_viewport(last_vp);
}
#endif /* USB_ENABLE_HID */


#ifdef USB_ENABLE_AUDIO
/* firmware/usbstack/usb_audio.c */
struct usb_audio_diag { unsigned init_ok, init_fail, disconnects, set_alt0, set_alt1, packets, short_pkts, err_pkts,
                        fb_sent, mixer_starts, set_freq; unsigned long last_freq; };   /* v83: same as usb_audio.c */
extern struct usb_audio_diag usb_audio_diag;
extern bool usb_audio_get_playing(void);
extern unsigned long usb_audio_get_playback_sampling_frequency(void);
extern int usb_audio_get_cur_volume(void);
extern int usb_audio_get_prebuffering(void);
extern bool usb_audio_get_underflow(void);
extern bool usb_audio_get_overflow(void);
extern int usb_audio_get_frames_dropped(void);
extern bool usb_audio_get_alloc_failed(void);
#endif

#ifdef USB_STATS
/* bcm overlay v76: what the link is doing, refreshed 4 times a second (Windows' copy dialog averages over seconds):
   the speed the computer negotiated, the real read / write rate over the last second, totals and peaks of this
   connection, the CPU clock, the battery, and the drive's own temperature when it reports one (SMART 194 / 190).
   The PP5022 has no temperature sensor, and on the 5G / 5.5G the PCF50605's battery-temperature input is wired to
   dock pin 21 (accessory identification), so there is no CPU or battery temperature to show. */
static struct {
    unsigned long long r0, w0;               /* counters when this connection started */
    unsigned long long r[5], w[5];           /* the last 5 samples, 0.25 s apart */
    long t[5];
    int n, k;
    unsigned peak_r, peak_w;                 /* KB/s */
    long smart_next;
    int temp;                                /* degrees C, -1 = not reported, -2 = not read yet */
} us;
static void usb_stats_start(void)
{
    memset(&us, 0, sizeof us);
    us.r0 = usb_stat_read_bytes; us.w0 = usb_stat_write_bytes;
    us.temp = -2;
}
static int usb_drive_temp(void)
{
    static struct ata_smart_values sv;
    int i;
    if (ata_read_smart(&sv, ATA_SMART_READ_DATA) != 0)
        return -1;
    for (i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; i++)
    {
        const struct ata_smart_attribute *a = &sv.vendor_attributes[i];
        if ((a->id == 194 || a->id == 190) && a->raw[0] > 0 && a->raw[0] < 100)
            return a->raw[0];
    }
    return -1;
}
static void fmt_bytes(char *b, size_t n, unsigned long long v)
{
    if (v >= (1ULL << 30)) snprintf(b, n, "%lu.%02lu GB", (unsigned long)(v >> 30), (unsigned long)(((v >> 20) & 1023) * 100 / 1024));
    else if (v >= (1ULL << 20)) snprintf(b, n, "%lu.%lu MB", (unsigned long)(v >> 20), (unsigned long)(((v >> 10) & 1023) * 10 / 1024));
    else snprintf(b, n, "%lu KB", (unsigned long)(v >> 10));
}
static void usb_stats_draw(const struct viewport *logo)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport vp;
    unsigned long long r = usb_stat_read_bytes, w = usb_stat_write_bytes;
    int i = us.k % 5, o, lh, y, bw;
    unsigned rr = 0, wr = 0;
    char a[24], b[24];
    us.r[i] = r; us.w[i] = w; us.t[i] = current_tick; us.k++;
    if (us.n < 5) us.n++;
    o = (us.k - us.n) % 5;                                  /* the oldest sample */
    if (us.n > 1 && us.t[i] != us.t[o])
    {
        long dt = us.t[i] - us.t[o];
        rr = (unsigned)((us.r[i] - us.r[o]) * HZ / dt / 1024);
        wr = (unsigned)((us.w[i] - us.w[o]) * HZ / dt / 1024);
        if (rr > us.peak_r) us.peak_r = rr;
        if (wr > us.peak_w) us.peak_w = wr;
    }
    if (us.temp == -2 || TIME_AFTER(current_tick, us.smart_next))
    {
        us.temp = usb_drive_temp();
        us.smart_next = current_tick + 20 * HZ;
    }
    /* v78: its own opaque panel over whatever the theme draws there (v76 used the theme's viewport: its backdrop and
       colours showed through, and some themes covered the numbers) */
    viewport_set_fullscreen(&vp, SCREEN_MAIN);
    lh = font_get(FONT_SYSFIXED)->height + 2;
    vp.x = 2; vp.width = LCD_WIDTH - 4;
    vp.y = logo->y + logo->height + 2;
#ifdef USB_ENABLE_HID
    if (global_settings.usb_hid)
        vp.y += font_get(FONT_UI)->height * 2;     /* below the keypad-mode line (title viewport) */
#endif
    vp.height = 10 * lh + 22;
    if (vp.y + vp.height > LCD_HEIGHT) vp.y = LCD_HEIGHT - vp.height;
    if (vp.y < 0) { vp.y = 0; vp.height = LCD_HEIGHT; }
    vp.font = FONT_SYSFIXED;
    vp.flags &= ~VP_FLAG_ALIGN_CENTER;
    d->set_viewport(&vp);
    d->set_drawmode(DRMODE_SOLID);
    d->set_foreground(LCD_RGBPACK(16, 20, 28));
    d->fillrect(0, 0, vp.width, vp.height);          /* solid: no backdrop through it */
    d->set_foreground(LCD_RGBPACK(90, 110, 140));
    d->drawrect(0, 0, vp.width, vp.height);
    d->set_foreground(LCD_RGBPACK(255, 255, 255));
    d->set_background(LCD_RGBPACK(16, 20, 28));
    y = 3;
    d->putsxyf(4, y, "Link    %s", !usb_inserted() ? "USB power only (no computer)" :
               usb_drv_port_speed() ? "USB 2.0 High Speed (480 Mbit/s)" : "USB Full Speed (12 Mbit/s)");
    y += lh;
    fmt_bytes(a, sizeof a, r - us.r0); fmt_bytes(b, sizeof b, w - us.w0);
    d->putsxyf(4, y, "Read   %3u.%u MB/s   peak %3u.%u   total %s", rr / 1024, rr % 1024 * 10 / 1024,
               us.peak_r / 1024, us.peak_r % 1024 * 10 / 1024, a);
    y += lh;
    d->putsxyf(4, y, "Write  %3u.%u MB/s   peak %3u.%u   total %s", wr / 1024, wr % 1024 * 10 / 1024,
               us.peak_w / 1024, us.peak_w % 1024 * 10 / 1024, b);
    y += lh;
    /* a bar for the current rate: full = 30 MB/s */
    {
        int fr, fw;
        bw = vp.width - 8;
        fr = (int)((long)bw * (rr > 30720 ? 30720 : rr) / 30720);
        fw = (int)((long)bw * (wr > 30720 ? 30720 : wr) / 30720);
        d->set_foreground(LCD_RGBPACK(90, 110, 140));
        d->drawrect(4, y, bw, 5); d->drawrect(4, y + 7, bw, 5);
        d->set_foreground(LCD_RGBPACK(80, 200, 120)); d->fillrect(4, y, fr, 5);      /* read: green */
        d->set_foreground(LCD_RGBPACK(240, 160, 60)); d->fillrect(4, y + 7, fw, 5);  /* write: orange */
        d->set_foreground(LCD_RGBPACK(255, 255, 255));
        y += 16;
    }
    d->putsxyf(4, y, "CPU    %ld MHz", cpu_frequency / 1000000);
    y += lh;
#if CONFIG_CHARGING
    d->putsxyf(4, y, "Battery %d mV  %d %%%s", battery_voltage(), battery_level(), charging_state() ? "  charging" : "");
#else
    d->putsxyf(4, y, "Battery %d mV  %d %%", battery_voltage(), battery_level());
#endif
    y += lh;
    if (us.temp >= 0)
        d->putsxyf(4, y, "Drive  %d C  (SMART)", us.temp);
    else
        d->putsxyf(4, y, "Drive  temperature not reported");
    y += lh;
    d->putsxyf(4, y, "CPU / battery temperature: no sensor");
    y += lh;
    d->putsxyf(4, y, "USB    %s%s%s%s", usb_core_driver_enabled(USB_DRIVER_MASS_STORAGE) ? "storage " : "",
#ifdef USB_ENABLE_AUDIO
               usb_core_driver_enabled(USB_DRIVER_AUDIO) ? "DAC " : "",
#else
               "",
#endif
#ifdef USB_ENABLE_HID
               usb_core_driver_enabled(USB_DRIVER_HID) ? "HID " : "",
#else
               "",
#endif
#ifdef USB_ENABLE_IAP
               usb_core_driver_enabled(USB_DRIVER_IAP) ? "iAP" : ""
#else
               ""
#endif
               );
#ifdef USB_ENABLE_AUDIO
    y += lh;
    if (usb_core_driver_enabled(USB_DRIVER_AUDIO))  /* v77: Mass Storage + USB DAC; v83: what the computer did */
    {
        d->putsxyf(4, y, "DAC %s %luHz open %u pkts %u err %u snd %u", !usb_audio_get_active() ? "off" :
                   usb_audio_get_playing() ? "play" : "idle", usb_audio_get_playback_sampling_frequency(),
                   usb_audio_diag.set_alt1, usb_audio_diag.packets, usb_audio_diag.err_pkts, usb_audio_diag.mixer_starts);
    }
#endif
    d->update_viewport();
    d->set_viewport(NULL);
}
#endif

#ifdef USB_STATS
static const struct viewport *usb_stats_logo;
#endif
static void handle_usb_events(struct viewport *title)
{
#if (CONFIG_STORAGE & STORAGE_MMC) && !defined(SIMULATOR)
    int next_update = 0;
#endif /* STORAGE_MMC */
    int button;

    /* Don't return until we get SYS_USB_DISCONNECTED */
    while(1)
    {
#ifndef USB_ENABLE_HID
        (void)title;
#else
        if (global_settings.usb_hid)
        {
#ifdef USB_STATS
            /* v81: with USB HID on (Rockbox's default) this branch runs, and v76-v80 drew the statistics only in the
               other one: they never appeared */
#endif
            button = get_hid_usb_action();
#ifdef USB_STATS
            usb_stats_draw(usb_stats_logo);
#endif
            if (button == ACTION_USB_HID_MODE_SWITCH_NEXT ||
                button == ACTION_USB_HID_MODE_SWITCH_PREV)
                draw_usb_keypad_mode(title);
        }
        else
#endif
        {
            /* hid emits the event in get_action */
            send_event(GUI_EVENT_ACTIONUPDATE, NULL);
#ifdef USB_STATS
            usb_stats_draw(usb_stats_logo);         /* after the event: anything it redrew is covered again */
            button = button_get_w_tmo(HZ/4);
#else
            button = button_get_w_tmo(HZ/2);
#endif
        }
        if (button == SYS_USB_DISCONNECTED)
            return;
        if (button == SYS_CHARGER_DISCONNECTED)
            reset_runtime();

/* USB-MMC bridge can report activity */
#if (CONFIG_STORAGE & STORAGE_MMC) && !defined(SIMULATOR)
        if(TIME_AFTER(current_tick,next_update))
        {
            if(usb_inserted()) {
                led(mmc_usb_active(HZ));
            }
            next_update=current_tick+HZ/2;
        }
#endif /* STORAGE_MMC */
    }
}

static void usb_screen_fix_viewports(struct screen *screen,
        struct usb_screen_vps_t *usb_screen_vps)
{
    int logo_width, logo_height;
    struct viewport *parent = &usb_screen_vps->parent;
    struct viewport *logo = &usb_screen_vps->logo;

#ifdef HAVE_REMOTE_LCD
    if (screen->screen_type == SCREEN_REMOTE)
    {
        logo_width = BMPWIDTH_remote_usblogo;
        logo_height = BMPHEIGHT_remote_usblogo;
    }
    else
#endif
    {
        logo_width = BMPWIDTH_usblogo;
        logo_height = BMPHEIGHT_usblogo;
    }

#ifdef USB_STATS
    /* v82: no theme on the main screen while the statistics are up: the theme's status bar / USB graphics were drawn
       over (and under) the panel at every refresh.  The theme comes back when the USB screen ends. */
    viewportmanager_theme_enable(screen->screen_type, screen->screen_type != SCREEN_MAIN, parent);
#else
    viewportmanager_theme_enable(screen->screen_type, true, parent);
#endif

    if (logo_width  > parent->width)
        logo_width  = parent->width;
    if (logo_height > parent->height)
        logo_height = parent->height;

    *logo = *parent;
    logo->x = parent->x + parent->width - logo_width;
#ifdef HAVE_LCD_SPLIT
    switch (statusbar_position(screen))
    {
         /* start beyond split */
         case STATUSBAR_OFF:
             logo->y = parent->y + LCD_SPLIT_POS;
             break;
         case STATUSBAR_TOP:
             logo->y = parent->y + LCD_SPLIT_POS - STATUSBAR_HEIGHT;
             break;
         /* start at the top for maximum space */
         default:
             logo->y = parent->y;
             break;
    }
#else
    logo->y = parent->y + (parent->height - logo_height) / 2;
#endif
#ifdef USB_STATS
    if (screen->screen_type == SCREEN_MAIN)
        logo->y = parent->y + 2;              /* v76: room for the statistics below it */
#endif
    logo->width = logo_width;
    logo->height = logo_height;

#ifdef USB_ENABLE_HID
    if (global_settings.usb_hid)
    {
        struct viewport *title = &usb_screen_vps->title;
        int char_height = font_get(parent->font)->height;
        *title = *parent;
        title->y = logo->y + logo->height + char_height;
        title->height = char_height;
        /* try to fit logo and title to parent */
        if (parent->y + parent->height < title->y + title->height)
        {
            logo->y = parent->y;
            title->y = parent->y + logo->height;
        }

        int i =0, langid = LANG_USB_KEYPAD_MODE;
        while (langid >= 0) /* ensure the USB mode strings get cached */
        {
            font_getstringsize(str(langid), NULL, NULL, title->font);
            langid = keypad_mode_name_get(i++);
        }
    }
#endif
}

static void usb_screens_draw(struct usb_screen_vps_t *usb_screen_vps_ar)
{
    struct viewport *last_vp;
    static const struct bitmap* logos[NB_SCREENS] = {
        &bm_usblogo,
#ifdef HAVE_REMOTE_LCD
        &bm_remote_usblogo,
#endif
    };
    FOR_NB_SCREENS(i)
    {
        struct screen *screen = &screens[i];
        struct usb_screen_vps_t *usb_screen_vps = &usb_screen_vps_ar[i];
        struct viewport *parent = &usb_screen_vps->parent;
        struct viewport *logo = &usb_screen_vps->logo;

        last_vp = screen->set_viewport(parent);
        screen->clear_viewport();
        screen->backlight_on();
        screen->set_viewport(logo);
        screen->bmp(logos[i], 0, 0);
        screen->set_viewport(last_vp);
    }
#ifdef USB_ENABLE_HID
    if (global_settings.usb_hid)
        draw_usb_keypad_mode(&usb_screen_vps_ar[SCREEN_MAIN].title);
#endif
}

void gui_usb_screen_run(bool early_usb, intptr_t seqnum)
{
#ifdef SIMULATOR /* the sim allows toggling USB fast enough to overflow viewportmanagers stack */
    static bool in_usb_screen = false;
    if (in_usb_screen)
        return;
    in_usb_screen = true;
#endif

    struct usb_screen_vps_t usb_screen_vps_ar[NB_SCREENS];
    struct viewport *title = NULL;
#if defined HAVE_TOUCHSCREEN
    enum touchscreen_mode old_mode = touchscreen_get_mode();

    /* TODO: Paint buttons on screens OR switch to point mode and use
     * touchscreen as a touchpad to move the host's mouse cursor */
    touchscreen_set_mode(TOUCHSCREEN_BUTTON);
#endif

    push_current_activity(ACTIVITY_USBSCREEN);

#ifdef USB_ENABLE_HID
    usb_keypad_mode = global_settings.usb_keypad_mode;
    title = &usb_screen_vps_ar[SCREEN_MAIN].title;
#else
    title = NULL;
#endif

    FOR_NB_SCREENS(i)
    {
        struct screen *screen = &screens[i];
        /* we might be coming from anywhere, and the originating screen
         * can't be practically expected to cleanup the UI because
         * we're invoked via default_event_handler(), therefore we make a
         * generic cleanup here */
        screen->set_viewport(NULL);
        screen->scroll_stop();
        usb_screen_fix_viewports(screen, &usb_screen_vps_ar[i]);
    }

#if 0 /* handled in usb_screen_fix_viewports() */
    /* update the UI before disabling fonts, this maximizes the propability
     * that font cache lookups succeed during USB */
    send_event(GUI_EVENT_ACTIONUPDATE, NULL);
#endif

    if(!early_usb)
    {
        /* The font system leaves the .fnt fd's open, so we need for force close them all */
        font_disable_all();
    }
    usb_acknowledge(SYS_USB_CONNECTED_ACK, seqnum);
#ifdef USB_STATS
    fb_data *saved_backdrop = lcd_get_backdrop();
    lcd_set_backdrop(NULL);                           /* v82: a plain background (the theme's picture comes back after) */
#endif
    usb_screens_draw(usb_screen_vps_ar);
#ifdef USB_STATS
    usb_stats_logo = &usb_screen_vps_ar[SCREEN_MAIN].logo;
    usb_stats_start();
#endif
    handle_usb_events(title);
#ifdef USB_STATS
    lcd_set_backdrop(saved_backdrop);
#endif

#if defined(USB_ENABLE_HID)
    if (global_settings.usb_hid)
        screens[SCREEN_MAIN].scroll_stop_viewport(title);
    if (global_settings.usb_keypad_mode != usb_keypad_mode)
    {
        global_settings.usb_keypad_mode = usb_keypad_mode;
        settings_save();
    }
#endif

#ifdef HAVE_TOUCHSCREEN
    touchscreen_set_mode(old_mode);
#endif

    if(!early_usb)
    {
        font_enable_all();
        /* Not pretty, reload all settings so fonts are loaded again correctly */
        settings_apply(true);
        /* Reload playlist */
        playlist_resume();
    }

    FOR_NB_SCREENS(i)
    {
        screens[i].backlight_on();
        viewportmanager_theme_undo(i, false);
    }

    pop_current_activity();
#ifdef SIMULATOR
    in_usb_screen = false;
#endif
}

#if defined(IPOD_VIDEO) && defined(USB_ENABLE_AUDIO) && !defined(SIMULATOR)
/* bcm overlay v77: Settings > General Settings > System > USB > USB DAC Status -- what the USB DAC is doing (in a
   charge-only USB DAC preset the iPod keeps its normal screens, so this is where to look) */
#include "pcm.h"
#include "pcm_sink.h"
#include "sound.h"
extern bool usb_charge_suspend;
int bcm_usb_dac_status(void)
{
    static const char *const preset[] = { "Mass storage", "Mass storage + USB DAC", "USB DAC (charging)",
                                          "USB DAC (no charging)", "Car / accessory", "Charge only", "Manual" };
    static const char *const st[] = { "not set up", "addressed", "configured" };
    /* v83: everything the DAC driver saw, on screen and (when the disk is Rockbox's: not in mass storage) once a second
       in /.rockbox/usb_dac_log.txt -- send that file */
    extern unsigned usb_core_diag_resets, usb_core_diag_configs;
    extern int usb_core_diag_state(void);
    extern bool usb_exclusive_storage_pending(void);
    extern int usb_iso_rx_mult;
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport vp;
    int lh = font_get(FONT_SYSFIXED)->height + 1, under = 0, over = 0, fd = -1;
    long t0 = current_tick, next_log = current_tick;
    viewport_set_defaults(&vp, SCREEN_MAIN);
    vp.font = FONT_SYSFIXED;
    while (1)
    {
        int y = 2, a, nd = sound_numdecimals(SOUND_VOLUME), vol = sound_current(SOUND_VOLUME);
        enum pcm_sink_ids sk = pcm_current_sink();
        bool on = usb_audio_get_active();
        const char *out = sk == PCM_SINK_BUILTIN ? "headphones/dock" :
#ifdef HAVE_PIEZO_SPEAKER
                          sk == PCM_SINK_PIEZO ? "click speaker" :
#endif
                          "USB accessory";
        if (on && usb_audio_get_underflow()) under++;
        if (on && usb_audio_get_overflow()) over++;
        d->set_viewport(&vp);
        d->clear_viewport();
        d->putsxyf(4, y, "USB DAC status  (MENU: back, SELECT: rx mode)"); y += lh + 3;
        d->putsxyf(4, y, "Preset  %s", preset[global_settings.bcm_usb_connect % 7]); y += lh;
        d->putsxyf(4, y, "USB     %s, %s, %s", usb_inserted() ? "computer" : charger_inserted() ? "power only" : "unplugged",
                   usb_drv_port_speed() ? "high speed" : "full speed", st[usb_core_diag_state()]); y += lh;
        d->putsxyf(4, y, "        resets %u  configs %u  functions %s%s%s", usb_core_diag_resets, usb_core_diag_configs,
                   usb_core_driver_enabled(USB_DRIVER_MASS_STORAGE) ? "storage " : "",
                   usb_core_driver_enabled(USB_DRIVER_AUDIO) ? "DAC " : "(no DAC) ",
                   usb_core_driver_enabled(USB_DRIVER_HID) ? "HID" : ""); y += lh;
        d->putsxyf(4, y, "DAC     %s  buffer %s  set up %u/%u", on ? "active" : "inactive",
                   usb_audio_get_alloc_failed() ? "FAILED" : "ok", usb_audio_diag.init_ok, usb_audio_diag.init_fail); y += lh;
        d->putsxyf(4, y, "Stream  %s  (opened %u closed %u)  %lu Hz", usb_audio_get_playing() ? "playing" : "closed",
                   usb_audio_diag.set_alt1, usb_audio_diag.set_alt0, usb_audio_diag.last_freq); y += lh;
        d->putsxyf(4, y, "Packets %u  (short %u, errors %u)  feedback %u", usb_audio_diag.packets, usb_audio_diag.short_pkts,
                   usb_audio_diag.err_pkts, usb_audio_diag.fb_sent); y += lh;
        d->putsxyf(4, y, "Buffer  %d  sound started %u  under %d over %d drop %d", usb_audio_get_prebuffering(),
                   usb_audio_diag.mixer_starts, under, over, usb_audio_get_frames_dropped()); y += lh;
        a = on ? usb_audio_get_cur_volume() : 0;
        d->putsxyf(4, y, "Volume  computer %d dB, iPod %d dB", nd ? a / 10 : a, nd ? vol / 10 : vol); y += lh;
        d->putsxyf(4, y, "Output  %s   rx mode %s", out, usb_iso_rx_mult ? "A (Mult 1)" : "B (Mult 0)"); y += lh;
        d->putsxyf(4, y, "Power   %s, battery %d %%", usb_charge_suspend ? "not from USB" :
                   charging_state() ? "charging" : "not charging", battery_level()); y += lh + 3;
        d->putsxyf(4, y, fd >= 0 ? "logging to /.rockbox/usb_dac_log.txt" :
                         usb_exclusive_storage_pending() ? "(no log: the drive belongs to the computer)" : "");
        d->update_viewport();
        d->set_viewport(NULL);
        if (!TIME_BEFORE(current_tick, next_log) && !usb_exclusive_storage_pending())
        {
            next_log = current_tick + HZ;
            if (fd < 0)
            {
                fd = open(ROCKBOX_DIR "/usb_dac_log.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
                if (fd >= 0) fdprintf(fd, "---- USB DAC status, preset %s, DAC speed setting %s\n",
                                      preset[global_settings.bcm_usb_connect % 7],
                                      global_settings.bcm_usb_dac_hs ? "high" : "full");
            }
            if (fd >= 0)
            {
                fdprintf(fd, "%4ld s: usb=%d speed=%s state=%s resets=%u configs=%u storage=%d dac=%d hid=%d | active=%d "
                         "alloc_fail=%d init=%u/%u alt1=%u alt0=%u playing=%d freq=%lu | pkts=%u short=%u err=%u fb=%u | "
                         "buf=%d started=%u under=%d over=%d drop=%d | vol pc=%d ipod=%d | out=%s rx=%d\n",
                         (current_tick - t0) / HZ, usb_inserted(), usb_drv_port_speed() ? "high" : "full",
                         st[usb_core_diag_state()], usb_core_diag_resets, usb_core_diag_configs,
                         usb_core_driver_enabled(USB_DRIVER_MASS_STORAGE), usb_core_driver_enabled(USB_DRIVER_AUDIO),
                         usb_core_driver_enabled(USB_DRIVER_HID), on, usb_audio_get_alloc_failed(),
                         usb_audio_diag.init_ok, usb_audio_diag.init_fail, usb_audio_diag.set_alt1, usb_audio_diag.set_alt0,
                         usb_audio_get_playing(), usb_audio_diag.last_freq, usb_audio_diag.packets,
                         usb_audio_diag.short_pkts, usb_audio_diag.err_pkts, usb_audio_diag.fb_sent,
                         usb_audio_get_prebuffering(), usb_audio_diag.mixer_starts, under, over,
                         usb_audio_get_frames_dropped(), a, vol, out, usb_iso_rx_mult);
            }
        }
        a = get_action(CONTEXT_STD, HZ / 4);
        if (a == ACTION_STD_CANCEL || a == ACTION_STD_MENU)
            break;
        if (a == ACTION_STD_OK)
        {   /* v83: the other receive mode; it is used from the next connection */
            usb_iso_rx_mult = !usb_iso_rx_mult;
            splash(HZ * 2, "Receive mode changed: unplug and plug in again");
            if (fd >= 0) fdprintf(fd, "rx mode -> %s\n", usb_iso_rx_mult ? "A (Mult 1)" : "B (Mult 0)");
        }
        if (default_event_handler(a) == SYS_USB_CONNECTED)
        {
            if (fd >= 0) close(fd);
            return 1;
        }
    }
    if (fd >= 0) close(fd);
    return 0;
}
#endif
