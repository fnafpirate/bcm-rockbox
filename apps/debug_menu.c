/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Heikki Hannikainen
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

#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string-extra.h>
#include "lcd.h"
#include "lang.h"
#include "menu.h"
#include "debug_menu.h"
#include "kernel.h"
#include "action.h"
#include "debug.h"
#include "thread.h"
#include "powermgmt.h"
#include "system.h"
#include "font.h"
#include "backlight.h"
#include "audio.h"
#include "settings.h"
#include "list.h"
#include "dir.h"
#include "panic.h"
#include "screens.h"
#include "misc.h"
#include "splash.h"
#include "shortcuts.h"
#include "dircache.h"
#include "viewport.h"
#ifdef HAVE_TAGCACHE
#include "tagcache.h"
#endif
#ifdef HAVE_REMOTE_LCD
#include "lcd-remote.h"
#endif
#include "crc32.h"
#include "logf.h"
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
#include "disk.h"
#include "adc.h"
#include "usb.h"
#include "rtc.h"
#include "storage.h"
#include "fs_defines.h"
#include "eeprom_24cxx.h"
#if (CONFIG_STORAGE & STORAGE_MMC) || (CONFIG_STORAGE & STORAGE_SD)
#include "sdmmc.h"
#endif
#if (CONFIG_STORAGE & STORAGE_ATA)
#include "ata.h"
#endif
#endif /* CONFIG_PLATFORM & PLATFORM_NATIVE */
#include "power.h"

#if ((CONFIG_PLATFORM & PLATFORM_NATIVE) || defined(SAMSUNG_YPR0) || defined(SAMSUNG_YPR1) \
        || defined(SONY_NWZ_LINUX)) \
    && CONFIG_TUNER != 0
#include "tuner.h"
#include "radio.h"
#endif

#include "scrollbar.h"
#include "peakmeter.h"
#include "skin_engine/skin_engine.h"
#include "logfdisp.h"
#include "core_alloc.h"
#include "pcmbuf.h"
#include "buffering.h"
#include "playback.h"
#if defined(HAVE_SPDIF_OUT) || defined(HAVE_SPDIF_IN)
#include "spdif.h"
#endif
#ifdef IRIVER_H300_SERIES
#include "pcf50606.h"   /* for pcf50606_read */
#endif
#ifdef IAUDIO_X5
#include "ds2411.h"
#endif
#include "hwcompat.h"
#include "button.h"
#if CONFIG_RTC == RTC_PCF50605
#include "pcf50605.h"
#endif
#include "appevents.h"

#if defined(HAVE_AS3514) && CONFIG_CHARGING
#include "ascodec.h"
#endif

#ifdef IPOD_NANO2G
#include "pmu-target.h"
#endif

#ifdef SANSA_CONNECT
#include "avr-sansaconnect.h"
#endif

#ifdef HAVE_USBSTACK
#include "usb_core.h"
#ifdef USB_ENABLE_AUDIO
#include "../usbstack/usb_audio.h"
#endif
#endif

#include "talk.h"

#if defined(HAVE_DEVICEDATA)// && !defined(SIMULATOR)
#include "devicedata.h"
#endif

#if defined(HAVE_BOOTDATA) && !defined(SIMULATOR)
#include "bootdata.h"
#include "multiboot.h"
#include "rbpaths.h"
#include "pathfuncs.h"
#include "rb-loader.h"
#endif

#if defined(IPOD_6G) && !defined(SIMULATOR)
#include "norboot-target.h"
#endif

#if defined(IPOD_ACCESSORY_PROTOCOL)
#include "iap.h"
#endif
#ifdef IPOD_VIDEO
#include "rbunicode.h"
#include "bcm_subs.h"                     /* v73: soft subtitles */
#include "bcm_dock.h"                     /* v74: docks */
#endif

#if defined(HIBY_LINUX) && !defined(SIMULATOR)
#include <sys/sysinfo.h>
#endif

#define SCREEN_MAX_CHARS (LCD_WIDTH / SYSFONT_WIDTH)

static const char* threads_getname(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    int *x_offset = (int*) data;

#if NUM_CORES > 1
    if (selected_item < (int)NUM_CORES)
    {
        struct core_debug_info coreinfo;
        core_get_debug_info(selected_item, &coreinfo);
        snprintf(buffer, buffer_len, "Idle (%2d): %2d%%", selected_item,
                 coreinfo.idle_stack_usage);
        return buffer;
    }

    selected_item -= NUM_CORES;
#endif

    const char *fmtstr = "%2d: ---";

    struct thread_debug_info threadinfo;
    if (thread_get_debug_info(selected_item, &threadinfo) > 0)
    {
        fmtstr = "%2d:" IF_COP(" (%d)") " %s%n" IF_PRIO(" %2d %2d")
                 IFN_SDL(" %2d%% %2d%%") " %s";
    }
    int status_len;
    size_t len = snprintf(buffer, buffer_len, fmtstr,
             selected_item,
             IF_COP(threadinfo.core,)
             threadinfo.statusstr,
             &status_len,
             IF_PRIO(threadinfo.base_priority, threadinfo.current_priority,)
             IFN_SDL(threadinfo.stack_usage_cur, threadinfo.stack_usage,)
             threadinfo.name);

    int start = 0;
#if LCD_WIDTH <= 128
    if (len >= SCREEN_MAX_CHARS)
    {
        int ch_offset = (*x_offset)%(len-1);
        int rem = SCREEN_MAX_CHARS - (len - ch_offset);
        if (rem > 0)
            ch_offset -= rem;

        if (ch_offset > 0)
        {
            /* don't scroll the # and status */
            status_len++;
            if ((unsigned int)ch_offset + status_len < buffer_len)
                memmove(&buffer[ch_offset], &buffer[0], status_len);
            start = ch_offset;
        }
    }
#else
    (void) x_offset;
    (void) len;
#endif
    return &buffer[start];
}

static int dbg_threads_action_callback(int action, struct gui_synclist *lists)
{

    if (action == ACTION_NONE)
    {
        return ACTION_REDRAW;
    }
#if LCD_WIDTH <= 128
    int *x_offset = ((int*) lists->data);
#if CONFIG_CPU == AS3525v2
    if (action == ACTION_STD_MENU)
    {
        *x_offset += 1;
        action = ACTION_REDRAW;
    }
    else if (IS_SYSEVENT(action))
    {
        return ACTION_REDRAW;
    }
    else if (action == ACTION_STD_OK)
    {
        *x_offset = 0;
        action = ACTION_REDRAW;
    }
#else
    if (action == ACTION_STD_OK)
    {
        *x_offset += 1;
        action = ACTION_REDRAW;
    }
    else if (IS_SYSEVENT(action))
    {
        return ACTION_REDRAW;
    }
    else if (action != ACTION_UNKNOWN)
    {
        *x_offset = 0;
    }
#endif
#else
    (void) lists;
#endif
    return action;
}
/* Test code!!! */
static bool dbg_os(void)
{
    struct simplelist_info info;
    int xoffset = 0;

    simplelist_info_init(&info, IF_COP("Core and ") "Stack usage:",
                         MAXTHREADS IF_COP( + NUM_CORES ), &xoffset);
    info.scroll_all = false;
    info.action_callback = dbg_threads_action_callback;
    info.get_name = threads_getname;
    return simplelist_show_list(&info);
}

#ifdef __linux__
#include "cpuinfo-linux.h"

#define MAX_STATES 16
static struct time_state states[MAX_STATES];

static const char* get_cpuinfo(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    (void)data;(void)buffer_len;
    const char* text;
    long time, diff;
    struct cpuusage us;
    static struct cpuusage last_us;
    int state_count = *(int*)data;

    if (cpuusage_linux(&us) != 0)
        return NULL;

    switch(selected_item)
    {
        case 0:
            diff = abs(last_us.usage - us.usage);
            sprintf(buffer, "Usage: %ld.%02ld%% (%c %ld.%02ld)",
                                    us.usage/100, us.usage%100,
                                    (us.usage >= last_us.usage) ? '+':'-',
                                    diff/100, diff%100);
            last_us.usage = us.usage;
            return buffer;
        case 1:
            text = "User";
            time = us.utime;
            diff = us.utime - last_us.utime;
            last_us.utime = us.utime;
            break;
        case 2:
            text = "Sys";
            time = us.stime;
            diff = us.stime - last_us.stime;
            last_us.stime = us.stime;
            break;
        case 3:
            text = "Real";
            time = us.rtime;
            diff = us.rtime - last_us.rtime;
            last_us.rtime = us.rtime;
            break;
        case 4:
            return "*** Per CPU freq stats ***";
        default:
        {
            int cpu = (selected_item - 5) / (state_count + 1);
            int cpu_line = (selected_item - 5) % (state_count + 1);

            /* scaling info */
            int min_freq = min_scaling_frequency(cpu);
            int cur_freq = current_scaling_frequency(cpu);
            /* fallback if scaling frequency is not available */
            if(cur_freq <= 0)
                cur_freq = frequency_linux(cpu);
            int max_freq = max_scaling_frequency(cpu);
            char governor[20];
            bool have_governor = current_scaling_governor(cpu, governor, sizeof(governor));
            if(cpu_line == 0)
            {
                sprintf(buffer,
                        " CPU%d: %s: %d/%d/%d MHz",
                        cpu,
                        have_governor ? governor : "Min/Cur/Max freq",
                        min_freq > 0 ? min_freq/1000 : -1,
                        cur_freq > 0 ? cur_freq/1000 : -1,
                        max_freq > 0 ? max_freq/1000 : -1);
            }
            else
            {
                cpustatetimes_linux(cpu, states, ARRAYLEN(states));
                snprintf(buffer, buffer_len, "   %ld %ld",
                            states[cpu_line-1].frequency,
                            states[cpu_line-1].time);
            }
            return buffer;
        }
    }
    sprintf(buffer, "%s: %ld.%02lds (+ %ld.%02ld)", text,
                    time / us.hz, time % us.hz,
                    diff / us.hz, diff % us.hz);
    return buffer;
}

static int cpuinfo_cb(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_NONE)
        action = ACTION_REDRAW;
    return action;
}

static bool dbg_cpuinfo(void)
{
    struct simplelist_info info;
    int cpu_count = MAX(cpucount_linux(), 1);
    int state_count = cpustatetimes_linux(0, states, ARRAYLEN(states));
    printf("%s(): %d %d\n", __func__, cpu_count, state_count);
    simplelist_info_init(&info, "CPU info:", 5 + cpu_count*(state_count+1), &state_count);
    info.get_name = get_cpuinfo;
    info.action_callback = cpuinfo_cb;
    info.timeout = HZ;
    info.scroll_all = true;
    return simplelist_show_list(&info);
}

#endif

static unsigned int ticks, freq_sum;
#ifndef CPU_MULTI_FREQUENCY
static unsigned int boost_ticks;
#endif

static void dbg_audio_task(void)
{
#ifdef CPUFREQ_NORMAL
#ifndef CPU_MULTI_FREQUENCY
    if(FREQ > CPUFREQ_NORMAL)
        boost_ticks++;
#endif
    freq_sum += FREQ/1000000; /* in MHz */
#endif
    ticks++;
}

static bool dbg_buffering_thread(void)
{
    int button;
    int line;
    bool done = false;
    size_t bufused;
    size_t bufsize = pcmbuf_get_bufsize();
    int pcmbufdescs = pcmbuf_descs();
    struct buffering_debug d;
    size_t filebuflen = audio_get_filebuflen();
    /* This is a size_t, but call it a long so it puts a - when it's bad. */
#if LCD_WIDTH > 96
    #define STR_DATAREM "data_rem"
    const char * const fmt_used = "%s: %6ld/%ld";
#else /* clipzip, ?*/
    #define STR_DATAREM "remain"
    const char * const fmt_used = "%s:%ld/%ld";
#endif

#ifndef CPU_MULTI_FREQUENCY
    boost_ticks = 0;
#endif
    ticks = freq_sum = 0;

    tick_add_task(dbg_audio_task);

    FOR_NB_SCREENS(i)
        screens[i].setfont(FONT_SYSFIXED);

    while(!done)
    {
        button = get_action(CONTEXT_STD,HZ/5);
        switch(button)
        {
            case ACTION_STD_NEXT:
                audio_next();
                break;
            case ACTION_STD_PREV:
                audio_prev();
                break;
            case ACTION_STD_CANCEL:
                done = true;
                break;
        }

        buffering_get_debugdata(&d);
        bufused = bufsize - pcmbuf_free();

        FOR_NB_SCREENS(i)
        {
            line = 0;
            screens[i].clear_display();


            screens[i].putsf(0, line++, fmt_used, "pcm", (long) bufused, (long) bufsize);

            gui_scrollbar_draw(&screens[i],0, line*SYSFONT_HEIGHT, screens[i].lcdwidth, 6,
                               bufsize, 0, bufused, HORIZONTAL);
            line++;

            screens[i].putsf(0, line++, fmt_used, "alloc", audio_filebufused(),
                            (long) filebuflen);

#if LCD_HEIGHT > 80 || (defined(HAVE_REMOTE_LCD) && LCD_REMOTE_HEIGHT > 80)
            if (screens[i].lcdheight > 80)
            {
                gui_scrollbar_draw(&screens[i],0, line*SYSFONT_HEIGHT, screens[i].lcdwidth, 6,
                                   filebuflen, 0, audio_filebufused(), HORIZONTAL);
                line++;

                screens[i].putsf(0, line++, fmt_used, "real", (long)d.buffered_data,
                                (long)filebuflen);

                gui_scrollbar_draw(&screens[i],0, line*SYSFONT_HEIGHT, screens[i].lcdwidth, 6,
                                   filebuflen, 0, (long)d.buffered_data, HORIZONTAL);
                line++;
            }
#endif

            screens[i].putsf(0, line++, fmt_used, "usefl", (long)(d.useful_data),
                                                       (long)filebuflen);

#if LCD_HEIGHT > 80 || (defined(HAVE_REMOTE_LCD) && LCD_REMOTE_HEIGHT > 80)
            if (screens[i].lcdheight > 80)
            {
                gui_scrollbar_draw(&screens[i],0, line*SYSFONT_HEIGHT, screens[i].lcdwidth, 6,
                                   filebuflen, 0, d.useful_data, HORIZONTAL);
                line++;
            }
#endif

            screens[i].putsf(0, line++, "%s: %ld", STR_DATAREM, (long)d.data_rem);

            screens[i].putsf(0, line++, "track count: %2u", audio_track_count());

            screens[i].putsf(0, line++, "handle count: %d", (int)d.num_handles);

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
            screens[i].putsf(0, line++, "cpu freq: %3dMHz",
                             (int)((FREQ + 500000) / 1000000));
#endif

            if (ticks > 0)
            {
                int avgclock   = freq_sum * 10 / ticks;      /* in 100 kHz */
#ifdef CPU_MULTI_FREQUENCY
                int boostquota = (avgclock * 100 - CPUFREQ_NORMAL/1000) /
                    ((CPUFREQ_MAX - CPUFREQ_NORMAL) / 1000000); /* in 0.1 % */
#else
                int boostquota = boost_ticks * 1000 / ticks; /* in 0.1 % */
#endif
                screens[i].putsf(0, line++, "boost:%3d.%d%% (%d.%dMHz)",
                                 boostquota/10, boostquota%10, avgclock/10, avgclock%10);
            }

            screens[i].putsf(0, line++, "pcmbufdesc: %2d/%2d",
                             pcmbuf_used_descs(), pcmbufdescs);
            screens[i].putsf(0, line++, "watermark: %6d",
                             (int)(d.watermark));

            screens[i].update();
        }
    }

    tick_remove_task(dbg_audio_task);

    FOR_NB_SCREENS(i)
        screens[i].setfont(FONT_UI);

    return false;
#undef STR_DATAREM
}

#ifdef BUFLIB_DEBUG_PRINT
static const char* bf_getname(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    (void)data;
    core_print_block_at(selected_item, buffer, buffer_len);
    return buffer;
}

static int bf_action_cb(int action, struct gui_synclist* list)
{
    if (action == ACTION_STD_OK)
    {
        if (gui_synclist_get_sel_pos(list) == 0 && core_test_free())
        {
            splash(HZ, "Freed test handle. New alloc should trigger compact");
        }
        else
        {
            splash(HZ/1, "Attempting a 64k allocation");
            int handle = core_alloc(64<<10);
            splash(HZ/2, (handle > 0) ? "Success":"Fail");
            /* for some reason simplelist doesn't allow adding items here if
             * info.get_name is given, so use normal list api */
            gui_synclist_set_nb_items(list, core_get_num_blocks());
            core_free(handle);
        }
        action = ACTION_REDRAW;
    }
    return action;
}

static bool dbg_buflib_allocs(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "mem allocs", core_get_num_blocks(), NULL);
    info.get_name = bf_getname;
    info.action_callback = bf_action_cb;
    info.timeout = HZ;
    return simplelist_show_list(&info);
}
#endif /* BUFLIB_DEBUG_PRINT */

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
static const char* dbg_partitions_getname(int selected_item, void *data,
                                          char *buffer, size_t buffer_len)
{
    (void)data;
    int partition = selected_item/2;

    struct partinfo p;
    if (!disk_partinfo(partition, &p))
        return buffer;

    // XXX fix this up to use logical sector size
    // XXX and if mounted, show free info...
    if (selected_item%2)
    {
        snprintf(buffer, buffer_len, "   T:%x %lu MB", p.type,
                 (unsigned long)(p.size / ( 2048 / ( SECTOR_SIZE / 512 ))));
    }
    else
    {
        snprintf(buffer, buffer_len, "P%d: S:%llx", partition, (unsigned long long)p.start);
    }
    return buffer;
}

static bool dbg_partitions(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "Partition Info", NUM_DRIVES * MAX_PARTITIONS_PER_DRIVE, NULL);
    info.selection_size = 2;
    info.scroll_all = true;
    info.get_name = dbg_partitions_getname;
    return simplelist_show_list(&info);
}
#endif /* PLATFORM_NATIVE */

#if defined(CPU_COLDFIRE) && defined(HAVE_SPDIF_OUT)
static bool dbg_spdif(void)
{
    int line;
    unsigned int control;
    int x;
    char *s;
    int category;
    int generation;
    unsigned int interruptstat;
    bool valnogood, symbolerr, parityerr;
    bool done = false;
    bool spdif_src_on;
    int spdif_source = spdif_get_output_source(&spdif_src_on);
    spdif_set_output_source(AUDIO_SRC_SPDIF IF_SPDIF_POWER_(, true));

    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);

#ifdef HAVE_SPDIF_POWER
    spdif_power_enable(true); /* We need SPDIF power for both sending & receiving */
#endif

    while (!done)
    {
        line = 0;

        control = EBU1RCVCCHANNEL1;
        interruptstat = INTERRUPTSTAT;
        INTERRUPTCLEAR = 0x03c00000;

        valnogood = (interruptstat & 0x01000000)?true:false;
        symbolerr = (interruptstat & 0x00800000)?true:false;
        parityerr = (interruptstat & 0x00400000)?true:false;

        lcd_putsf(0, line++, "Val: %s Sym: %s Par: %s",
                 valnogood?"--":"OK",
                 symbolerr?"--":"OK",
                 parityerr?"--":"OK");

        lcd_putsf(0, line++, "Status word: %08x", (int)control);

        line++;

        x = control >> 31;
        lcd_putsf(0, line++, "PRO: %d (%s)",
                 x, x?"Professional":"Consumer");

        x = (control >> 30) & 1;
        lcd_putsf(0, line++, "Audio: %d (%s)",
                 x, x?"Non-PCM":"PCM");

        x = (control >> 29) & 1;
        lcd_putsf(0, line++, "Copy: %d (%s)",
                 x, x?"Permitted":"Inhibited");

        x = (control >> 27) & 7;
        switch(x)
        {
        case 0:
            s = "None";
            break;
        case 1:
            s = "50/15us";
            break;
        default:
            s = "Reserved";
            break;
        }
        lcd_putsf(0, line++, "Preemphasis: %d (%s)", x, s);

        x = (control >> 24) & 3;
        lcd_putsf(0, line++, "Mode: %d", x);

        category = (control >> 17) & 127;
        switch(category)
        {
        case 0x00:
            s = "General";
            break;
        case 0x40:
            s = "Audio CD";
            break;
        default:
            s = "Unknown";
        }
        lcd_putsf(0, line++, "Category: 0x%02x (%s)", category, s);

        x = (control >> 16) & 1;
        generation = x;
        if(((category & 0x70) == 0x10) ||
           ((category & 0x70) == 0x40) ||
           ((category & 0x78) == 0x38))
        {
            generation = !generation;
        }
        lcd_putsf(0, line++, "Generation: %d (%s)",
                 x, generation?"Original":"No ind.");

        x = (control >> 12) & 15;
        lcd_putsf(0, line++, "Source: %d", x);


        x = (control >> 8) & 15;
        switch(x)
        {
        case 0:
            s = "Unspecified";
            break;
        case 8:
            s = "A (Left)";
            break;
        case 4:
            s = "B (Right)";
            break;
        default:
            s = "";
            break;
        }
        lcd_putsf(0, line++, "Channel: %d (%s)", x, s);

        x = (control >> 4) & 15;
        switch(x)
        {
        case 0:
            s = "44.1kHz";
            break;
        case 0x4:
            s = "48kHz";
            break;
        case 0xc:
            s = "32kHz";
            break;
        }
        lcd_putsf(0, line++, "Frequency: %d (%s)", x, s);

        x = (control >> 2) & 3;
        lcd_putsf(0, line++, "Clock accuracy: %d", x);
        line++;

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
        lcd_putsf(0, line++, "Measured freq: %ldHz",
                 spdif_measure_frequency());
#endif

        lcd_update();

        if (action_userabort(HZ/10))
            break;
    }

    spdif_set_output_source(spdif_source IF_SPDIF_POWER_(, spdif_src_on));

#ifdef HAVE_SPDIF_POWER
    spdif_power_enable(global_settings.spdif_enable);
#endif

    lcd_setfont(FONT_UI);
    return false;
}
#endif /* CPU_COLDFIRE */

#if (CONFIG_RTC == RTC_PCF50605) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
static bool dbg_pcf(void)
{
    int line;

    lcd_setfont(FONT_SYSFIXED);
    lcd_clear_display();

    while(1)
    {
        line = 0;

        lcd_putsf(0, line++, "DCDC1:  %02x", pcf50605_read(0x1b));
        lcd_putsf(0, line++, "DCDC2:  %02x", pcf50605_read(0x1c));
        lcd_putsf(0, line++, "DCDC3:  %02x", pcf50605_read(0x1d));
        lcd_putsf(0, line++, "DCDC4:  %02x", pcf50605_read(0x1e));
        lcd_putsf(0, line++, "DCDEC1: %02x", pcf50605_read(0x1f));
        lcd_putsf(0, line++, "DCDEC2: %02x", pcf50605_read(0x20));
        lcd_putsf(0, line++, "DCUDC1: %02x", pcf50605_read(0x21));
        lcd_putsf(0, line++, "DCUDC2: %02x", pcf50605_read(0x22));
        lcd_putsf(0, line++, "IOREGC: %02x", pcf50605_read(0x23));
        lcd_putsf(0, line++, "D1REGC: %02x", pcf50605_read(0x24));
        lcd_putsf(0, line++, "D2REGC: %02x", pcf50605_read(0x25));
        lcd_putsf(0, line++, "D3REGC: %02x", pcf50605_read(0x26));
        lcd_putsf(0, line++, "LPREG1: %02x", pcf50605_read(0x27));
        lcd_update();
        if (action_userabort(HZ/10))
        {
            lcd_setfont(FONT_UI);
            return false;
        }
    }

    lcd_setfont(FONT_UI);
    return false;
}
#endif

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
static bool dbg_cpufreq(void)
{
    int line;
    int button;
    int x = 0;
    bool done = false;

    lcd_setfont(FONT_SYSFIXED);
    lcd_clear_display();

    while(!done)
    {
        line = 0;

        int temp = FREQ / 1000;
        lcd_putsf(x, line++, "Frequency: %ld.%ld MHz", temp / 1000, temp % 1000);
        lcd_putsf(x, line++, "boost_counter: %d", get_cpu_boost_counter());

#ifdef HAVE_ADJUSTABLE_CPU_VOLTAGE
        extern int get_cpu_voltage_setting(void);
        temp = get_cpu_voltage_setting();
        lcd_putsf(x, line++, "CPU voltage: %d.%03dV", temp / 1000, temp % 1000);
#endif

        lcd_update();
        button = get_action(CONTEXT_STD,HZ/10);

        switch(button)
        {
            case ACTION_STD_PREV:
                cpu_boost(true);
                break;

            case ACTION_STD_NEXT:
                cpu_boost(false);
                break;
            case ACTION_STD_MENU:
                x--;
                break;
            case ACTION_STD_OK:
                x = 0;
                while (get_cpu_boost_counter() > 0)
                    cpu_boost(false);
                set_cpu_frequency(CPUFREQ_DEFAULT);
                break;

            case ACTION_STD_CANCEL:
                done = true;;
        }
        lcd_clear_display();
    }
    lcd_setfont(FONT_UI);
    return false;
}
#endif /* HAVE_ADJUSTABLE_CPU_FREQ */

#if defined(HAVE_TSC2100) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
#include "tsc2100.h"
static const char* tsc2100_debug_getname(int selected_item, void * data,
                                         char *buffer, size_t buffer_len)
{
    int *page = (int*)data;
    bool reserved = false;
    switch (*page)
    {
        case 0:
            if ((selected_item > 0x0a)  ||
                (selected_item == 0x04) ||
                (selected_item == 0x08))
                reserved = true;
            break;
        case 1:
            if ((selected_item > 0x05) ||
                (selected_item == 0x02))
                reserved = true;
            break;
        case 2:
            if (selected_item > 0x1e)
                reserved = true;
            break;
    }
    if (reserved)
        snprintf(buffer, buffer_len, "%02x: RSVD", selected_item);
    else
        snprintf(buffer, buffer_len, "%02x: %04x", selected_item,
                    tsc2100_readreg(*page, selected_item)&0xffff);
    return buffer;
}
static int tsc2100debug_action_callback(int action, struct gui_synclist *lists)
{
    int *page = (int*)lists->data;
    if (action == ACTION_STD_OK)
    {
        *page = (*page+1)%3;
        snprintf((char*)lists->title, 32, "tsc2100 registers - Page %d", *page);
        return ACTION_REDRAW;
    }
    return action;
}
static bool tsc2100_debug(void)
{
    int page = 0;
    char title[32];
    snprintf(title, 32, "tsc2100 registers - Page %d", page);
    struct simplelist_info info;
    simplelist_info_init(&info, title, 32, &page);
    info.timeout = HZ/100;
    info.get_name = tsc2100_debug_getname;
    info.action_callback= tsc2100debug_action_callback;
    return simplelist_show_list(&info);
}
#endif
#if (CONFIG_BATTERY_MEASURE != 0) && !defined(SIMULATOR)
/*
 * view_battery() shows a automatically scaled graph of the battery voltage
 * over time. Usable for estimating battery life / charging rate.
 * The power_history array is updated in power_thread of powermgmt.c.
 */

#define BAT_LAST_VAL  MIN(LCD_WIDTH, POWER_HISTORY_LEN)
#define BAT_TSPACE    20
#define BAT_YSPACE    (LCD_HEIGHT - BAT_TSPACE)


static bool view_battery(void)
{
    extern struct battery_tables_t device_battery_tables; /* powermgmt.c */
    unsigned short *power_history = device_battery_tables.history;
    int view = 0;
    int i, x, y, z, y1, y2, grid, graph;
    unsigned short maxv, minv;
    lcd_setfont(FONT_SYSFIXED);

    while(1)
    {
        lcd_clear_display();
        switch (view) {
            case 0: /* voltage history graph */
                /* Find maximum and minimum voltage for scaling */
                minv = power_history[0];
                maxv = minv + 1;
                for (i = 1; i < BAT_LAST_VAL && power_history[i]; i++) {
                    if (power_history[i] > maxv)
                        maxv = power_history[i];
                    if (power_history[i] < minv)
                        minv = power_history[i];
                }
                /* print header */
#if (CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE)
                /* adjust grid scale */
                if ((maxv - minv) > 50)
                    grid = 50;
                else
                    grid = 5;

                lcd_putsf(0, 0, "%s %d.%03dV", "Battery", power_history[0] / 1000,
                         power_history[0] % 1000);
                lcd_putsf(0, 1, "%d.%03d-%d.%03dV (%2dmV)",
                          minv / 1000, minv % 1000, maxv / 1000, maxv % 1000,
                          grid);
#elif (CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE)
                /* adjust grid scale */
                if ((maxv - minv) > 10)
                    grid = 10;
                else
                    grid = 1;
                lcd_putsf(0, 0, "%s %d%%", "Battery", power_history[0]);
                lcd_putsf(0, 1, "%d%%-%d%% (%d %%)", minv, maxv, grid);
#endif

                i = 1;
                while ((y = (minv - (minv % grid)+i*grid)) < maxv)
                {
                    graph = ((y-minv)*BAT_YSPACE)/(maxv-minv);
                    graph = LCD_HEIGHT-1 - graph;

                    /* draw dotted horizontal grid line */
                    for (x=0; x<LCD_WIDTH;x=x+2)
                        lcd_drawpixel(x,graph);

                    i++;
                }

                x = 0;
                /* draw plot of power history
                 * skip empty entries
                 */
                for (i = BAT_LAST_VAL - 1; i > 0; i--)
                {
                    if (power_history[i] && power_history[i-1])
                    {
                        y1 = (power_history[i] - minv) * BAT_YSPACE /
                            (maxv - minv);
                        y1 = MIN(MAX(LCD_HEIGHT-1 - y1, BAT_TSPACE),
                                 LCD_HEIGHT-1);
                        y2 = (power_history[i-1] - minv) * BAT_YSPACE /
                            (maxv - minv);
                        y2 = MIN(MAX(LCD_HEIGHT-1 - y2, BAT_TSPACE),
                                 LCD_HEIGHT-1);

                        lcd_set_drawmode(DRMODE_SOLID);

                        /* make line thicker */
                        lcd_drawline(((x*LCD_WIDTH)/(BAT_LAST_VAL)),
                                     y1,
                                     (((x+1)*LCD_WIDTH)/(BAT_LAST_VAL)),
                                     y2);
                        lcd_drawline(((x*LCD_WIDTH)/(BAT_LAST_VAL))+1,
                                     y1+1,
                                     (((x+1)*LCD_WIDTH)/(BAT_LAST_VAL))+1,
                                     y2+1);
                        x++;
                    }
                }
                break;

            case 1: /* status: */
#if CONFIG_CHARGING >= CHARGING_MONITOR
                lcd_putsf(0, 0, "Pwr status: %s",
                         charging_state() ? "charging" : "discharging");
#else
                lcd_putsf(0, 0, "Pwr status: %s", "unknown");
#endif
                battery_read_info(&y, &z);
                if (y > 0)
                    lcd_putsf(0, 1, "%s: %d.%03d V (%d %%)", "Battery", y / 1000, y % 1000, z);
                else if (z > 0)
                    lcd_putsf(0, 1, "%s: %d %%", "Battery", z);
#ifdef ADC_EXT_POWER
                y = (adc_read(ADC_EXT_POWER) * EXT_SCALE_FACTOR) / 1000;
                lcd_putsf(0, 2, "%s: %d.%03d V", "External", y / 1000, y % 1000);
#endif
#if CONFIG_CHARGING
#if defined IPOD_NANO || defined IPOD_VIDEO
                int usb_pwr  = (GPIOL_INPUT_VAL & 0x10)?true:false;
                int ext_pwr  = (GPIOL_INPUT_VAL & 0x08)?false:true;
                int dock     = (GPIOA_INPUT_VAL & 0x10)?true:false;
                int charging = (GPIOB_INPUT_VAL & 0x01)?false:true;
                int headphone= (GPIOA_INPUT_VAL & 0x80)?true:false;

                lcd_putsf(0, 3, "USB pwr:   %s",
                            usb_pwr ? "present" : "absent");
                lcd_putsf(0, 4, "EXT pwr:   %s",
                            ext_pwr ? "present" : "absent");
                lcd_putsf(0, 5, "%s:   %s", "Battery",
                    charging ? "charging" : (usb_pwr||ext_pwr) ? "charged" : "discharging");
                lcd_putsf(0, 6, "Dock mode: %s",
                         dock    ? "enabled" : "disabled");
                lcd_putsf(0, 7, "Headphone: %s",
                         headphone ? "connected" : "disconnected");
#ifdef IPOD_VIDEO
                if(probed_ramsize == 64)
                    x = (adc_read(ADC_4066_ISTAT) * 2400) / (1024 * 2);
                else
#endif
                    x = (adc_read(ADC_4066_ISTAT) * 2400) / (1024 * 3);
                lcd_putsf(0, 8, "Ibat: %d mA", x);
                lcd_putsf(0, 9, "Vbat * Ibat: %d mW", x * y / 1000);
#ifdef HAVE_BATTERY_SMOOTH_API
                {   /* bcm overlay v58: the smoothed reading */
                    struct battery_smooth_info bi;
                    battery_smooth_info(&bi);
                    lcd_putsf(0, 11, "Reading: %s  shown %d %%", bi.enabled ? "smooth" : "exact", battery_level());
                    lcd_putsf(0, 12, "Exact %d %%  compensated %d.%d %%", bi.raw_percent,
                              bi.est_permille / 10, bi.est_permille % 10);
                    lcd_putsf(0, 13, "Battery resistance: %d mOhm", bi.resistance_mohm);
                    lcd_putsf(0, 14, "Drop mV  light %d%s  boost %d%s", bi.sag_mv[0], bi.loads & 1 ? "*" : "",
                              bi.sag_mv[1], bi.loads & 2 ? "*" : "");
                    lcd_putsf(0, 15, "         play %d%s  video %d%s", bi.sag_mv[2], bi.loads & 4 ? "*" : "",
                              bi.sag_mv[3], bi.loads & 8 ? "*" : "");
                    lcd_putsf(0, 16, "Measured %d / %d / %d / %d times", bi.measured[0], bi.measured[1],
                              bi.measured[2], bi.measured[3]);
                    /* v59: Apple's ADC mode and battery curve */
                    lcd_putsf(0, 18, "Apple ADC: %d  scale %s %d + %d.%03d/cnt (%d)", bi.sub_raw, bi.cal_ok ? "" : "?",
                              bi.cal_a, bi.cal_b1000 / 1000, bi.cal_b1000 % 1000, bi.cal_n);
                    lcd_putsf(0, 19, "Curve: %s (Apple table %d, hw %08x)", bi.apple_curve ? "Apple" : "Rockbox",
                              bi.apple_table, bi.hw_rev);
                    lcd_putsf(0, 20, "0-50%%: %d %d %d %d %d %d", bi.curve_mv[0], bi.curve_mv[1], bi.curve_mv[2],
                              bi.curve_mv[3], bi.curve_mv[4], bi.curve_mv[5]);
                    lcd_putsf(0, 21, "60-100%%: %d %d %d %d %d", bi.curve_mv[6], bi.curve_mv[7], bi.curve_mv[8],
                              bi.curve_mv[9], bi.curve_mv[10]);
                }
#endif
#elif defined TOSHIBA_GIGABEAT_S
                int line = 3;
                unsigned int st;

                static const unsigned char * const chrgstate_strings[] =
                {
                    "Disabled",
                    "Error",
                    "Discharging",
                    "Precharge",
                    "Constant Voltage",
                    "Constant Current",
                    "<unknown>",
                };

                lcd_putsf(0, line++, "Charger: %s",
                         charger_inserted() ? "present" : "absent");

                st = power_input_status() &
                     (POWER_INPUT_CHARGER | POWER_INPUT_BATTERY);

                lcd_putsf(0, line++, "%.*s%.*s",
                         !!(st & POWER_INPUT_MAIN_CHARGER)*5, " Main",
                         !!(st & POWER_INPUT_USB_CHARGER)*4, " USB");

                y = ARRAYLEN(chrgstate_strings) - 1;

                switch (charge_state)
                {
                case CHARGE_STATE_DISABLED: y--;
                case CHARGE_STATE_ERROR:    y--;
                case DISCHARGING:           y--;
                case TRICKLE:               y--;
                case TOPOFF:                y--;
                case CHARGING:              y--;
                default:;
                }

                lcd_putsf(0, line++, "State: %s", chrgstate_strings[y]);

                lcd_putsf(0, line++, "%s Switch: %s", "Battery",
                         (st & POWER_INPUT_BATTERY) ? "On" : "Off");

                y = chrgraw_adc_voltage();
                lcd_putsf(0, line++, "CHRGRAW: %d.%03d V",
                         y / 1000, y % 1000);

                y = application_supply_adc_voltage();
                lcd_putsf(0, line++, "BP     : %d.%03d V",
                         y / 1000, y % 1000);

                y = battery_adc_charge_current();
                lcd_putsf(0, line++, "CHRGISN:% d mA", y);

                y = cccv_regulator_dissipation();
                lcd_putsf(0, line++, "P CCCV : %d mW", y);

                y = battery_charge_current();
                lcd_putsf(0, line++, "I Charge:% d mA", y);

                y = battery_adc_temp();

                if (y != INT_MIN) {
                    lcd_putsf(0, line++, "T %s: %d\u00b0C (%d\u00b0F)",
                              "Battery", y, (9*y + 160) / 5);
                } else {
                    /* Conversion disabled */
                    lcd_putsf(0, line++, "T %s: ?", "Battery");
                }
#elif defined(HAVE_AS3514) && CONFIG_CHARGING
                static const char * const chrgstate_strings[] =
                {
                    [CHARGE_STATE_DISABLED - CHARGE_STATE_DISABLED]= "Disabled",
                    [CHARGE_STATE_ERROR - CHARGE_STATE_DISABLED] = "Error",
                    [DISCHARGING - CHARGE_STATE_DISABLED]       = "Discharging",
                    [CHARGING - CHARGE_STATE_DISABLED]          = "Charging",
                };
                const char *str = NULL;

                lcd_putsf(0, 3, "Charger: %s",
                         charger_inserted() ? "present" : "absent");

                y = charge_state - CHARGE_STATE_DISABLED;
                if ((unsigned)y < ARRAYLEN(chrgstate_strings))
                    str = chrgstate_strings[y];

                lcd_putsf(0, 4, "State: %s",
                         str ? str : "<unknown>");

                lcd_putsf(0, 5, "CHARGER: %02X", ascodec_read_charger());
#elif defined(IPOD_NANO2G)
                y = pmu_read_battery_voltage();
                lcd_putsf(17, 1, "RAW: %d.%03d V", y / 1000, y % 1000);
                y = pmu_read_battery_current();
                lcd_putsf(0, 2, "%s current: %d mA", "Battery", y);
                lcd_putsf(0, 3, "PWRCON: %08x %08x", PWRCON, PWRCONEXT);
                lcd_putsf(0, 4, "CLKCON: %08x %03x %03x", CLKCON, CLKCON2, CLKCON3);
                lcd_putsf(0, 5, "PLL: %06x %06x %06x", PLL0PMS, PLL1PMS, PLL2PMS);
                x = pmu_read(0x1b) & 0xf;
                y = pmu_read(0x1a) * 25 + 625;
                lcd_putsf(0, 6, "AUTO: %x / %d mV", x, y);
                x = pmu_read(0x1f) & 0xf;
                y = pmu_read(0x1e) * 25 + 625;
                lcd_putsf(0, 7, "DOWN1: %x / %d mV", x, y);
                x = pmu_read(0x23) & 0xf;
                y = pmu_read(0x22) * 25 + 625;
                lcd_putsf(0, 8, "DOWN2: %x / %d mV", x, y);
                x = pmu_read(0x27) & 0xf;
                y = pmu_read(0x26) * 100 + 900;
                lcd_putsf(0, 9, "MEMLDO: %x / %d mV", x, y);
                for (i = 0; i < 6; i++)
                {
                    x = pmu_read(0x2e + (i << 1)) & 0xf;
                    y = pmu_read(0x2d + (i << 1)) * 100 + 900;
                    lcd_putsf(0, 10 + i, "LDO%d: %x / %d mV", i + 1, x, y);
                }
#elif defined(SANSA_CONNECT)
                lcd_putsf(0, 3, "Charger: %s",
                         charger_inserted() ? "present" : "absent");
                x = (avr_hid_hdq_read_short(HDQ_REG_TEMP) / 4) - 273;
                lcd_putsf(0, 4, "%s temperature: %d C", "Battery", x);
                x = (avr_hid_hdq_read_short(HDQ_REG_AI) * 357) / 200;
                lcd_putsf(0, 5, "%s current: %d.%01d mA", "Battery", x / 10, x % 10);
                x = (avr_hid_hdq_read_short(HDQ_REG_AP) * 292) / 20;
                lcd_putsf(0, 6, "Discharge power: %d.%01d mW", x / 10, x % 10);
                x = (avr_hid_hdq_read_short(HDQ_REG_SAE) * 292) / 2;
                lcd_putsf(0, 7, "Available energy: %d.%01d mWh", x / 10, x % 10);
#else
                lcd_putsf(0, 3, "Charger: %s",
                         charger_inserted() ? "present" : "absent");
#if defined(HAVE_USBSTACK) && defined(HAVE_USB_CHARGING_ENABLE)
                lcd_putsf(0, 4, "USB current limit: %d mA",
                          usb_charging_maxcurrent());
#endif /* HAVE_USB_CHARGING_ENABLE */
#endif /* target type */
#endif /* CONFIG_CHARGING */
                break;
            case 2: /* voltage deltas: */
#if (CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE)
                lcd_puts(0, 0, "Voltage deltas:");
                for (i = 0; i < POWER_HISTORY_LEN-1; i++) {
                    y = power_history[i] - power_history[i+1];
                    lcd_putsf(0, i+1, "-%d min: %c%d.%03d V", i,
                             (y < 0) ? '-' : ' ', ((y < 0) ? y * -1 : y) / 1000,
                             ((y < 0) ? y * -1 : y ) % 1000);
                }
#elif (CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE)
                lcd_puts(0, 0, "Percentage deltas:");
                for (i = 0; i < POWER_HISTORY_LEN-1; i++) {
                    y = power_history[i] - power_history[i+1];
                    lcd_putsf(0, i+1, "-%d min: %c%d%%", i,
                             (y < 0) ? '-' : ' ', ((y < 0) ? y * -1 : y));
                }
#endif
                break;

            case 3: /* remaining time estimation: */

#if (CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE)
                lcd_putsf(0, 5, "Last PwrHist: %d.%03dV",
                    power_history[0] / 1000,
                    power_history[0] % 1000);
#endif

                lcd_putsf(0, 6, "%s level: %d%%", "Battery", battery_level());

                int time_left = battery_time();
                if (time_left >= 0)
                    lcd_putsf(0, 7, "Est. remain: %d m", time_left);
                else
                    lcd_puts(0, 7, "Estimation n/a");

#if (CONFIG_BATTERY_MEASURE & CURRENT_MEASURE)
                lcd_putsf(0, 8, "%s current: %d mA", "Battery", battery_current());
#endif
                break;
        }

        lcd_update();

        switch(get_action(CONTEXT_STD,HZ/2))
        {
            case ACTION_STD_PREV:
                if (view)
                    view--;
                break;

            case ACTION_STD_NEXT:
                if (view < 3)
                    view++;
                break;

            case ACTION_STD_CANCEL:
                lcd_setfont(FONT_UI);
                return false;
        }
    }
    lcd_setfont(FONT_UI);
    return false;
}

#endif /* (CONFIG_BATTERY_MEASURE != 0)  */

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
#if (CONFIG_STORAGE & STORAGE_MMC) || (CONFIG_STORAGE & STORAGE_SD)

#if (CONFIG_STORAGE & STORAGE_MMC)
#define CARDTYPE "MMC"
#elif (CONFIG_STORAGE & STORAGE_SD)
#define CARDTYPE "microSD"
#endif

static int disk_callback(int btn, struct gui_synclist *lists)
{
    tCardInfo *card;
    int *cardnum = (int*)lists->data;
    unsigned char card_name[6];
    unsigned char pbuf[32];
    /* Casting away const is safe; the buffer is defined as non-const. */
    char *title = (char *)lists->title;
    static const unsigned char i_vmin[] = { 0, 1, 5, 10, 25, 35, 60, 100 };
    static const unsigned char i_vmax[] = { 1, 5, 10, 25, 35, 45, 80, 200 };
    static const unsigned char * const kbit_units[] = { "kBit/s", "MBit/s", "GBit/s" };
    static const unsigned char * const nsec_units[] = { "ns", "µs", "ms" };
#if (CONFIG_STORAGE & STORAGE_MMC)
    static const char * const mmc_spec_vers[] = { "1.0-1.2", "1.4", "2.0-2.2",
        "3.1-3.31", "4.0" };
#endif

    if ((btn == ACTION_STD_OK) || (btn == SYS_FS_CHANGED) || (btn == ACTION_REDRAW))
    {
#if NUM_DRIVES > 1
        if (btn == ACTION_STD_OK)
        {
            *cardnum ^= 0x1; /* change cards */
        }
#endif

        simplelist_reset_lines();

        card = card_get_info(*cardnum);

        if (card->initialized > 0)
        {
            unsigned i;
            for (i=0; i<sizeof(card_name); i++)
            {
                card_name[i] = card_extract_bits(card->cid, (103-8*i), 8);
            }
            strmemccpy(card_name, card_name, sizeof(card_name));
            simplelist_addline(
                    "%s Rev %d.%d", card_name,
                    (int) card_extract_bits(card->cid, 63, 4),
                    (int) card_extract_bits(card->cid, 59, 4));
            simplelist_addline(
                    "Prod: %d/%d",
#if (CONFIG_STORAGE & STORAGE_SD)
                    (int) card_extract_bits(card->cid, 11, 4),
                    (int) card_extract_bits(card->cid, 19, 8) + 2000
#elif (CONFIG_STORAGE & STORAGE_MMC)
                    (int) card_extract_bits(card->cid, 15, 4),
                    (int) card_extract_bits(card->cid, 11, 4) + 1997
#endif
                    );
            simplelist_addline(
#if (CONFIG_STORAGE & STORAGE_SD)
                    "Ser#: 0x%08lx",
                    card_extract_bits(card->cid, 55, 32)
#elif (CONFIG_STORAGE & STORAGE_MMC)
                    "Ser#: 0x%04lx",
                    card_extract_bits(card->cid, 47, 16)
#endif
                    );

            simplelist_addline("M=%02x, "
#if (CONFIG_STORAGE & STORAGE_SD)
                    "O=%c%c",
                    (int) card_extract_bits(card->cid, 127, 8),
                    (int) card_extract_bits(card->cid, 119, 8),
                    (int) card_extract_bits(card->cid, 111, 8)
#elif (CONFIG_STORAGE & STORAGE_MMC)
                    "O=%04x",
                    (int) card_extract_bits(card->cid, 127, 8),
                    (int) card_extract_bits(card->cid, 119, 16)
#endif
                    );

#if (CONFIG_STORAGE & STORAGE_MMC)
            int temp = card_extract_bits(card->csd, 125, 4);
            simplelist_addline(
                     "MMC v%s", temp < 5 ?
                            mmc_spec_vers[temp] : "?.?");
#endif
            simplelist_addline(
                    "Blocks: 0x%08lx", card->numblocks);
            output_dyn_value(pbuf, sizeof pbuf, card->speed / 1000,
                                            kbit_units, 3, false);
            simplelist_addline(
                    "Speed: %s", pbuf);
            output_dyn_value(pbuf, sizeof pbuf, card->taac,
                            nsec_units, 3, false);
            simplelist_addline(
                    "Taac: %s", pbuf);
            simplelist_addline(
                    "Nsac: %d clk", card->nsac);
            simplelist_addline(
                    "R2W: *%d", 1 << card->r2w_factor);
#if (CONFIG_STORAGE & STORAGE_SD)
            int csd_structure = card_extract_bits(card->csd, 127, 2);
            const char *ver;
            switch(csd_structure) {
            case 0:
                    ver = "1 (SD)";
                    break;
            case 1:
                    ver = "2 (SDHC/SDXC)";
                    break;
            case 2:
                    ver = "3 (SDUC)";
                    break;
            default:
                    ver = "Unknown";
                    break;
            }
            simplelist_addline("SDVer: %s", ver);
            if (csd_structure == 0) /* CSD version 1.0 */
#endif
            {
            simplelist_addline(
                    "IRmax: %d..%d mA",
                    i_vmin[card_extract_bits(card->csd, 61, 3)],
                    i_vmax[card_extract_bits(card->csd, 58, 3)]);
            simplelist_addline(
                    "IWmax: %d..%d mA",
                    i_vmin[card_extract_bits(card->csd, 55, 3)],
                    i_vmax[card_extract_bits(card->csd, 52, 3)]);
            }
#if (CONFIG_STORAGE & STORAGE_SD)
            /*
             * Most drivers don't read SCR so an all zero value means
             * the driver didn't populate it. Valid SCRs will have at
             * least the SD_BUS_WIDTHS field containing a nonzero value.
             */
            if (card->scr[0] || card->scr[1])
            {
                int scr_vers = (card->scr[1] >> 28) & 0xF;
                simplelist_addline("SCR version: %d", scr_vers);

                if (scr_vers == 0)
                {
                    bool supports_cmd23 = card->scr[1] & 0x2;
                    simplelist_addline("CMD23 supported: %s", supports_cmd23 ? "yes" : "no");
                }
            }
#endif
        }
        else if (card->initialized == 0)
        {
            simplelist_setline("Not Found!");
        }
#if (CONFIG_STORAGE & STORAGE_SD)
        else /* card->initialized < 0 */
        {
            simplelist_addline("Init Error! (%d)", card->initialized);
        }
#endif
        snprintf(title, 16, "[" CARDTYPE " %d]", *cardnum);
        gui_synclist_set_title(lists, title, Icon_NOICON);
        gui_synclist_set_nb_items(lists, simplelist_get_line_count());
        gui_synclist_select_item(lists, 0);
        btn = ACTION_REDRAW;
    }
    return btn;
}
#elif  (CONFIG_STORAGE & STORAGE_ATA)
static int disk_callback(int btn, struct gui_synclist *lists)
{
    static const char atanums[] = { " 0 1 2 3 4 5 6" };

    (void)lists;
    int i;
    char buf[128];
    unsigned short* identify_info = ata_get_identify();
    bool timing_info_present = false;
    (void)btn;

    simplelist_reset_lines();

    for (i=0; i < 20; i++)
        ((unsigned short*)buf)[i]=htobe16(identify_info[i+27]);
    buf[40]=0;
    /* kill trailing space */
    for (i=39; i && buf[i]==' '; i--)
        buf[i] = 0;
    simplelist_addline("Model: %s", buf);
    for (i=0; i < 10; i++)
        ((unsigned short*)buf)[i]=htobe16(identify_info[i+10]);
    buf[20]=0;
    /* kill trailing space */
    for (i=19; i && buf[i]==' '; i--)
        buf[i] = 0;
    simplelist_addline("Serial number: %s", buf);
    for (i=0; i < 4; i++)
        ((unsigned short*)buf)[i]=htobe16(identify_info[i+23]);
    buf[8]=0;
    simplelist_addline(
             "Firmware: %s", buf);

    uint64_t total_sectors = (identify_info[61] << 16) | identify_info[60];
#ifdef HAVE_LBA48
    if (identify_info[83] & 0x0400 && total_sectors == 0x0FFFFFFF)
        total_sectors = ((uint64_t)identify_info[103] << 48) |
                ((uint64_t)identify_info[102] << 32) |
                ((uint64_t)identify_info[101] << 16) |
                identify_info[100];
#endif

    uint32_t sector_size;

    /* Logical sector size > 512B ? */
    if ((identify_info[106] & 0xd000) == 0x5000) /* B14, B12 */
        sector_size = (identify_info[117] | (identify_info[118] << 16)) * 2;
    else
        sector_size = 512;

    total_sectors *= sector_size;   /* Convert to bytes */
    total_sectors /= (1024 * 1024); /* Convert to MB */

    simplelist_addline("Size: %lu MB", (unsigned long)total_sectors);
    simplelist_addline("Logical sector size: %lu B", sector_size);
#ifdef MAX_VIRT_SECTOR_SIZE
    simplelist_addline("Sector multiplier: %u", disk_get_sector_multiplier());
#endif

    if((identify_info[106] & 0xe000) == 0x6000) /* B14, B13 */
        sector_size *= BIT_N(identify_info[106] & 0x000f);
    simplelist_addline(
            "Physical sector size: %lu B", sector_size);

#ifndef HAVE_MULTIVOLUME
    // XXX this needs to be fixed for multi-volume setups
    sector_t free;
    volume_size( IF_MV(0,) NULL, &free );
    simplelist_addline(
            "Free: %lu MB", (unsigned long)(free / 1024));
#endif

    simplelist_addline("SSD detected: %s", ata_disk_isssd() ? "yes" : "no");
    simplelist_addline(
             "Spinup time: %d ms", storage_spinup_time() * (1000/HZ));
    i = identify_info[82] & (1<<3);
    simplelist_addline(
             "Power mgmt: %s", i ? "enabled" : "unsupported");
    i = identify_info[83] & (1<<3);
    simplelist_addline(
             "Adv Power mgmt: %s", i ? "enabled" : "unsupported");
    i = identify_info[83] & (1<<9);
    simplelist_addline(
             "Noise mgmt: %s", i ? "enabled" : "unsupported");
    i = identify_info[85] & (1<<0);
    simplelist_addline(
             "SMART: %s", i ? "enabled" : "unsupported");
    simplelist_addline(
             "Flush cache: %s", identify_info[83] & (1<<13) ? "extended" : identify_info[83] & (1<<12) ? "standard" : identify_info[80] >= (1<<5) ? "ATA-5" : "unsupported");
    i = identify_info[82] & (1<<6);
    simplelist_addline(
             "Read-ahead: %s", i ? "enabled" : "unsupported");
    timing_info_present = identify_info[53] & (1<<1);
    if(timing_info_present) {
        simplelist_addline(
                 "PIO modes: 0 1 2%.*s%.*s",
                 (identify_info[64] & (1<<0)) << 1, &atanums[3*2],
                 (identify_info[64] & (1<<1))     , &atanums[4*2]);
    }
    else {
        simplelist_setline(
                 "No PIO mode info");
    }
    timing_info_present = identify_info[53] & (1<<1);
    if(timing_info_present) {
        simplelist_addline(
                 "Cycle times %dns/%dns",
                 identify_info[67],
                 identify_info[68] );
    } else {
        simplelist_setline(
                 "No timing info");
    }

#ifdef HAVE_ATA_DMA
    if (identify_info[63] & (1<<0)) {
        simplelist_addline(
                 "MDMA modes:%.*s%.*s%.*s",
                  (identify_info[63] & (1<<0)) << 1, &atanums[0*2],
                  (identify_info[63] & (1<<1))     , &atanums[1*2],
                  (identify_info[63] & (1<<2)) >> 1, &atanums[2*2]);
        simplelist_addline(
                 "MDMA Cycle times %dns/%dns",
                 identify_info[65],
                 identify_info[66] );
    }
    else {
        simplelist_setline(
                "No MDMA mode info");
    }
    if (identify_info[53] & (1<<2)) {
        simplelist_addline(
                 "UDMA modes:%.*s%.*s%.*s%.*s%.*s%.*s%.*s",
                 (identify_info[88] & (1<<0)) << 1, &atanums[0*2],
                 (identify_info[88] & (1<<1))     , &atanums[1*2],
                 (identify_info[88] & (1<<2)) >> 1, &atanums[2*2],
                 (identify_info[88] & (1<<3)) >> 2, &atanums[3*2],
                 (identify_info[88] & (1<<4)) >> 3, &atanums[4*2],
                 (identify_info[88] & (1<<5)) >> 4, &atanums[5*2],
                 (identify_info[88] & (1<<6)) >> 5, &atanums[6*2]);
    }
    else {
        simplelist_setline("No UDMA mode info");
    }
#endif /* HAVE_ATA_DMA */
    timing_info_present = identify_info[53] & (1<<1);
    if(timing_info_present) {
        i = identify_info[49] & (1<<11);
        simplelist_addline(
            "IORDY support: %s", i ? "yes" : "no");
        i = identify_info[49] & (1<<10);
        simplelist_addline(
                "IORDY disable: %s", i ? "yes" : "no");
    } else {
        simplelist_setline("No timing info");
    }
    simplelist_addline(
             "Cluster size: %d bytes", volume_get_cluster_size(IF_MV(0)));
#ifdef HAVE_ATA_DMA
    i = ata_get_dma_mode();
    if (i == 0) {
        simplelist_setline("DMA not enabled");
    } else if (i == 0xff) {
        simplelist_setline("CE-ATA mode");
    } else {
        simplelist_addline(
                 "DMA mode: %s %c",
                (i & 0x40) ? "UDMA" : "MDMA",
                '0' + (i & 7));
    }
#endif /* HAVE_ATA_DMA */
    i = identify_info[83] & (1 << 2);
    simplelist_addline(
            "CFA compatible: %s", i ? "yes" : "no");
    i = identify_info[0] & (1 << 6);
    simplelist_addline(
            "Fixed device: %s", i ? "yes" : "no");
    i = identify_info[0] & (1 << 7);
    simplelist_addline(
            "Removeable media: %s", i ? "yes" : "no");

    return btn;
}

#ifdef HAVE_ATA_SMART
static struct ata_smart_values smart_data STORAGE_ALIGN_ATTR;

static const char * ata_smart_get_attr_name(unsigned char id)
{
    if (id == 1)    return "Raw Read Error Rate";
    if (id == 2)    return "Throughput Performance";
    if (id == 3)    return "Spin-Up Time";
    if (id == 4)    return "Start/Stop Count";
    if (id == 5)    return "Reallocated Sector Count";
    if (id == 7)    return "Seek Error Rate";
    if (id == 8)    return "Seek Time Performance";
    if (id == 9)    return "Power-On Hours Count";
    if (id == 10)   return "Spin-Up Retry Count";
    if (id == 12)   return "Power Cycle Count";
    if (id == 191)  return "G-Sense Error Rate";
    if (id == 192)  return "Power-Off Retract Count";
    if (id == 193)  return "Load/Unload Cycle Count";
    if (id == 194)  return "HDA Temperature";
    if (id == 195)  return "Hardware ECC Recovered";
    if (id == 196)  return "Reallocated Event Count";
    if (id == 197)  return "Current Pending Sector Count";
    if (id == 198)  return "Uncorrectable Sector Count";
    if (id == 199)  return "UDMA CRC Error Count";
    if (id == 200)  return "Write Error Rate";
    if (id == 201)  return "TA Counter Detected";
    if (id == 220)  return "Disk Shift";
    if (id == 222)  return "Loaded Hours";
    if (id == 223)  return "Load/Unload Retry Count";
    if (id == 224)  return "Load Friction";
    if (id == 225)  return "Load Cycle Count";
    if (id == 226)  return "Load-In Time";
    if (id == 240)  return "Transfer Error Rate";   /* Fujitsu */
    return "Unknown Attribute";
};

static int ata_smart_get_attr_rawfmt(unsigned char id)
{
    if (id == 3)      /* Spin-up time */
        return RAWFMT_RAW16_OPT_AVG16;

    if (id == 5 ||    /* Reallocated sector count */
        id == 196)    /* Reallocated event count */
        return RAWFMT_RAW16_OPT_RAW16;

    if (id == 190 ||  /* Airflow Temperature */
        id == 194)    /* HDA Temperature */
        return RAWFMT_TEMPMINMAX;

    return RAWFMT_RAW48;
};

static int ata_smart_attr_to_string(
                struct ata_smart_attribute *attr, char *str, int size)
{
    uint16_t w[3]; /* 3 words to store 6 bytes of raw data */
    char buf[size]; /* temp string to store attribute data */
    int len, slen;
    int id = attr->id;

    if (id == 0)
        return 0; /* null attribute */

    /* align and convert raw data */
    memcpy(w, attr->raw, 6);
    w[0] = letoh16(w[0]);
    w[1] = letoh16(w[1]);
    w[2] = letoh16(w[2]);

    len = snprintf(buf, size, ": %u,%u ", attr->current, attr->worst);

    switch (ata_smart_get_attr_rawfmt(id))
    {
        case RAWFMT_RAW16_OPT_RAW16:
            len += snprintf(buf+len, size-len, "%u", w[0]);
            if ((w[1] || w[2]) && (len < size))
                len += snprintf(buf+len, size-len, " %u %u", w[1],w[2]);
            break;

        case RAWFMT_RAW16_OPT_AVG16:
            len += snprintf(buf+len, size-len, "%u", w[0]);
            if (w[1] && (len < size))
                len += snprintf(buf+len, size-len, " Avg: %u", w[1]);
            break;

        case RAWFMT_TEMPMINMAX:
            len += snprintf(buf+len, size-len, "%u -/+: %u/%u", w[0],w[1],w[2]);
            break;

        case RAWFMT_RAW48:
        default: {
            uint32_t tmp;
            memcpy(&tmp, w, sizeof(tmp));
            /* shows first 4 bytes of raw data as uint32 LE,
               and the ramaining 2 bytes as uint16 LE */
            len += snprintf(buf+len, size-len, "%lu", letoh32(tmp));
            if (w[2] && (len < size))
                len += snprintf(buf+len, size-len, " %u", w[2]);
            break;
        }
    }
    /* ignore trailing \0 when truncated */
    if (len >= size) len = size-1;

    /* fill return string; when max. size is exceded: first truncate
       attribute name, then attribute data and finally attribute id */
    slen = snprintf(str, size, "%d ", id);
    if (slen < size) {
        /* maximum space disponible for attribute name,
           including initial space separator */
        int name_sz = size - (slen + len);
        if (name_sz > 1) {
            len = snprintf(str+slen, name_sz, " %s",
                           ata_smart_get_attr_name(id));
            if (len >= name_sz) len = name_sz-1;
            slen += len;
        }

        strmemccpy(str+slen, buf, size-slen);
    }

    return 1; /* ok */
}

static bool ata_smart_dump(void)
{
    int fd;

    fd = creat("/smart_data.bin", 0666);
    if(fd >= 0)
    {
        write(fd, &smart_data, sizeof(struct ata_smart_values));
        close(fd);
    }

    fd = creat("/smart_data.txt", 0666);
    if(fd >= 0)
    {
        int i;
        char buf[128];
        for (i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; i++)
        {
            if (ata_smart_attr_to_string(
                    &smart_data.vendor_attributes[i], buf, sizeof(buf)))
            {
                write(fd, buf, strlen(buf));
                write(fd, "\n", 1);
            }
        }
        close(fd);
    }

    return false;
}

static int ata_smart_callback(int btn, struct gui_synclist *lists)
{
    (void)lists;
    static bool read_done = false;

    if (btn == ACTION_STD_CANCEL)
    {
        read_done = false;
        return btn;
    }

    /* read S.M.A.R.T. data only on first redraw */
    if (!read_done)
    {
        int rc;
        memset(&smart_data, 0, sizeof(struct ata_smart_values));
        rc = ata_read_smart(&smart_data, ATA_SMART_READ_DATA);
        simplelist_reset_lines();
        if (rc == 0)
        {
            int i;
            char buf[SIMPLELIST_MAX_LINELENGTH];
            simplelist_setline("Id  Name:  Current,Worst  Raw");
            for (i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; i++)
            {
                if (ata_smart_attr_to_string(
                        &smart_data.vendor_attributes[i], buf, sizeof(buf)))
                {
                    simplelist_addline(buf);
                }
            }
        }
        else
        {
            simplelist_addline("ATA SMART error: %#x", rc);
        }
        read_done = true;
    }

    if (btn == ACTION_STD_CONTEXT)
    {
        splash(0, "Dumping data...");
        ata_smart_dump();
        splash(HZ, "SMART data dumped");
    }

    return btn;
}

static bool dbg_ata_smart(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "S.M.A.R.T. Data [CONTEXT to dump]", 1, NULL);
    info.action_callback = ata_smart_callback;
    info.scroll_all = true;
    return simplelist_show_list(&info);
}
#endif /* HAVE_ATA_SMART */
#else /* No SD, MMC or ATA */
static int disk_callback(int btn, struct gui_synclist *lists)
{
    (void)lists;
    struct storage_info info;
    storage_get_info(0,&info);
    simplelist_addline("Vendor: %s", info.vendor);
    simplelist_addline("Model: %s", info.product);
    simplelist_addline("Firmware: %s", info.revision);
    simplelist_addline(
            "Size: %lu MB", (unsigned long)(info.num_sectors*(info.sector_size/512)/2048));
    sector_t free;
    volume_size( IF_MV(0,) NULL, &free );
    simplelist_addline(
             "Free: %ld MB", free / 1024);
    simplelist_addline(
             "Cluster size: %d bytes", volume_get_cluster_size(IF_MV(0)));
    return btn;
}
#endif

#if  (CONFIG_STORAGE & STORAGE_ATA)
static bool dbg_identify_info(void)
{
    int fd = creat("/identify_info.bin", 0666);
    if(fd >= 0)
    {
        const unsigned short *identify_info = ata_get_identify();
#ifdef ROCKBOX_LITTLE_ENDIAN
        /* this is a pointer to a driver buffer so we can't modify it */
        for (int i = 0; i < ATA_IDENTIFY_WORDS; ++i)
        {
            unsigned short word = swap16(identify_info[i]);
            write(fd, &word, 2);
        }
#else
        write(fd, identify_info, ATA_IDENTIFY_WORDS*2);
#endif
        close(fd);
    }
    return false;
}
#endif

static bool dbg_disk_info(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "Disk Info", 1, NULL);
#if (CONFIG_STORAGE & STORAGE_MMC) || (CONFIG_STORAGE & STORAGE_SD)
    char title[16];
    int card = 0;
    info.callback_data = (void*)&card;
    info.title = title;
#endif
    info.action_callback = disk_callback;
    info.scroll_all = true;
    return simplelist_show_list(&info);
}
#endif /* PLATFORM_NATIVE */

#ifdef HAVE_DIRCACHE
static int dircache_callback(int btn, struct gui_synclist *lists)
{
    (void)lists;
    struct dircache_info info;
    dircache_get_info(&info);

    if (global_settings.dircache)
    {
        switch (btn)
        {
        case ACTION_STD_CONTEXT:
            splash(HZ/2, "Rebuilding cache");
            dircache_suspend();
            *(int *)lists->data = dircache_resume();
            /* Fallthrough */
        case ACTION_UNKNOWN:
            btn = ACTION_NONE;
            break;
    #ifdef DIRCACHE_DUMPSTER
        case ACTION_STD_OK:
            splash(0, "Dumping cache");
            dircache_dump();
            btn = ACTION_NONE;
            break;
    #endif /* DIRCACHE_DUMPSTER */
        case ACTION_STD_CANCEL:
            if (*(int *)lists->data > 0 && info.status == DIRCACHE_SCANNING)
            {
                splash(HZ, ID2P(LANG_SCANNING_DISK));
                btn = ACTION_NONE;
            }
            break;
        }
    }

    simplelist_reset_lines();

    simplelist_addline("Cache status: %s", info.statusdesc);
    simplelist_addline("Last size: %zu B", info.last_size);
    simplelist_addline("Size: %zu B", info.size);
    unsigned int utilized = info.size ? 1000ull*info.sizeused / info.size : 0;
    simplelist_addline("Used: %zu B (%u.%u%%)", info.sizeused,
                       utilized / 10, utilized % 10);
    simplelist_addline("Limit: %zu B", info.size_limit);
    simplelist_addline("Reserve: %zu/%zu B", info.reserve_used, info.reserve);
    long ticks = ALIGN_UP(info.build_ticks, HZ / 10);
    simplelist_addline("Scanning took: %ld.%ld s",
                       ticks / HZ, (ticks*10 / HZ) % 10);
    simplelist_addline("Entry count: %u", info.entry_count);

    return btn;
}

static bool dbg_dircache_info(void)
{
    struct simplelist_info info;
    int syncbuild = 0;
    simplelist_info_init(&info, "Dircache Info", 0, &syncbuild);
    info.action_callback = dircache_callback;
    info.scroll_all = true;
    return simplelist_show_list(&info);
}

#endif /* HAVE_DIRCACHE */

#ifdef HAVE_TAGCACHE
static int database_callback(int btn, struct gui_synclist *lists)
{
    (void)lists;
    struct tagcache_stat *stat = tagcache_get_stat();
    static bool synced = false;
    static int update_entries = 0;

    simplelist_reset_lines();

    simplelist_addline("Initialized: %s",
             stat->initialized ? "Yes" : "No");
    simplelist_addline("DB %s: %s", "Ready",
             stat->ready ? "Yes" : "No");
    simplelist_addline("DB Path: %s", stat->db_path);
    simplelist_addline("RAM Cache: %s",
             stat->ramcache ? "Yes" : "No");
    simplelist_addline("RAM: %d/%d B",
             stat->ramcache_used, stat->ramcache_allocated);
    simplelist_addline("Total entries: %d",
                       stat->total_entries);
    simplelist_setline("Progress:");
    simplelist_addline(" %d%% (%d entries)",
                       stat->progress, stat->processed_entries);
    simplelist_setline("Curfile:");
    simplelist_addline(" %s", stat->curentry ? stat->curentry : "---");
    simplelist_addline("Commit step: %d",
             stat->commit_step);
    simplelist_addline("Commit delayed: %s",
             stat->commit_delayed ? "Yes" : "No");

    simplelist_addline("Queue length: %d",
             stat->queue_length);

    if (synced)
    {
        synced = false;
        tagcache_screensync_event();
    }

    if (!btn && stat->curentry)
    {
        synced = true;
        if (update_entries <= stat->processed_entries)
        {
            update_entries = stat->processed_entries + 100;
            return ACTION_REDRAW;
        }
        return ACTION_NONE;
    }

    if (btn == ACTION_STD_CANCEL)
    {
        update_entries = 0;
        tagcache_screensync_enable(false);
    }
    return btn;
}
static bool dbg_tagcache_info(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "Database Info", 0, NULL);
    info.action_callback = database_callback;
    info.scroll_all = true;

    /* Don't do nonblock here, must give enough processing time
       for tagcache thread. */
    /* info.timeout = TIMEOUT_NOBLOCK; */
    info.timeout = 1;
    tagcache_screensync_enable(true);
    return simplelist_show_list(&info);
}
#endif

#if defined CPU_COLDFIRE
static bool dbg_save_roms(void)
{
    int fd;
    int oldmode = system_memory_guard(MEMGUARD_NONE);

#if defined(IRIVER_H100_SERIES)
    fd = creat("/internal_rom_000000-1FFFFF.bin", 0666);
#elif defined(IRIVER_H300_SERIES)
    fd = creat("/internal_rom_000000-3FFFFF.bin", 0666);
#elif defined(IAUDIO_X5) || defined(IAUDIO_M5) || defined(IAUDIO_M3)
    fd = creat("/internal_rom_000000-3FFFFF.bin", 0666);
#elif defined(MPIO_HD200) || defined(MPIO_HD300)
    fd = creat("/internal_rom_000000-1FFFFF.bin", 0666);
#endif
    if(fd >= 0)
    {
        write(fd, (void *)0, FLASH_SIZE);
        close(fd);
    }
    system_memory_guard(oldmode);

#ifdef HAVE_EEPROM
    fd = creat("/internal_eeprom.bin", 0666);
    if (fd >= 0)
    {
        int old_irq_level;
        char buf[EEPROM_SIZE];
        int err;

        old_irq_level = disable_irq_save();

        err = eeprom_24cxx_read(0, buf, sizeof buf);

        restore_irq(old_irq_level);

        if (err)
            splashf(HZ*3, "Eeprom read failure (%d)", err);
        else
        {
            write(fd, buf, sizeof buf);
        }

        close(fd);
    }
#endif

    return false;
}
#elif defined(CPU_PP) && !(CONFIG_STORAGE & STORAGE_SD)
static bool dbg_save_roms(void)
{
    int fd = creat("/internal_rom_000000-0FFFFF.bin", 0666);
    if(fd >= 0)
    {
        write(fd, (void *)0x20000000, FLASH_SIZE);
        close(fd);
    }

    return false;
}
#elif CONFIG_CPU == AS3525v2 || CONFIG_CPU == AS3525
static bool dbg_save_roms(void)
{
    int fd = creat("/rom.bin", 0666);
    if(fd >= 0)
    {
        write(fd, (void *)0x80000000, 0x20000);
        close(fd);
    }

    return false;
}
#elif CONFIG_CPU == IMX31L
bool __dbg_dvfs_dptc(void);
static bool dbg_save_roms(void)
{
    int fd = creat("/flash_rom_A0000000-A01FFFFF.bin", 0666);
    if (fd >= 0)
    {
        write(fd, (void*)0xa0000000, FLASH_SIZE);
        close(fd);
    }

    return false;
}
#elif defined(CPU_TCC780X)
static bool dbg_save_roms(void)
{
    int fd = creat("/eeprom_E0000000-E0001FFF.bin", 0666);
    if (fd >= 0)
    {
        write(fd, (void*)0xe0000000, 0x2000);
        close(fd);
    }

    return false;
}
#elif CONFIG_CPU == RK27XX
static bool dbg_save_roms(void)
{
    char buf[0x200];

    int fd = creat("/rom.bin", 0666);
    if(fd < 0)
        return false;

    for(int addr = 0; addr < 0x2000; addr += sizeof(buf))
    {
        int old_irq = disable_irq_save();

        /* map rom at 0 */
        SCU_REMAP = 0;
        commit_discard_idcache();

        /* copy rom */
        memcpy((void *)buf, (void *)addr, sizeof(buf));

        /* map iram back at 0 */
        SCU_REMAP = 0xdeadbeef;
        commit_discard_idcache();

        restore_irq(old_irq);

        write(fd, (void *)buf, sizeof(buf));
    }
    close(fd);

    return false;
}
#endif /* CPU */

#ifndef SIMULATOR
#if CONFIG_TUNER

#ifdef CONFIG_TUNER_MULTI
static int tuner_type = 0;
#define IF_TUNER_TYPE(type) if(tuner_type==type)
#else
#define IF_TUNER_TYPE(type)
#endif

static int radio_callback(int btn, struct gui_synclist *lists)
{
    (void)lists;
    if (btn == ACTION_STD_CANCEL)
        return btn;
    simplelist_reset_lines();
    simplelist_setline("HW detected: yes");

#if (CONFIG_TUNER & LV24020LP)
    simplelist_addline(
                        "CTRL_STAT: %02X", lv24020lp_get(LV24020LP_CTRL_STAT) );
    simplelist_addline(
                        "RADIO_STAT: %02X", lv24020lp_get(LV24020LP_REG_STAT) );
    simplelist_addline(
                        "MSS_FM: %d kHz", lv24020lp_get(LV24020LP_MSS_FM) );
    simplelist_addline(
                        "MSS_IF: %d Hz", lv24020lp_get(LV24020LP_MSS_IF) );
    simplelist_addline(
                        "MSS_SD: %d Hz", lv24020lp_get(LV24020LP_MSS_SD) );
    simplelist_addline(
                        "if_set: %d Hz", lv24020lp_get(LV24020LP_IF_SET) );
    simplelist_addline(
                        "sd_set: %d Hz", lv24020lp_get(LV24020LP_SD_SET) );
#endif /* LV24020LP */
#if (CONFIG_TUNER & TEA5767)
    struct tea5767_dbg_info nfo;
    tea5767_dbg_info(&nfo);
    simplelist_setline("Philips regs:");
    simplelist_addline(
             "   %s: %02X %02X %02X %02X %02X", "Read",
             (unsigned)nfo.read_regs[0], (unsigned)nfo.read_regs[1],
             (unsigned)nfo.read_regs[2], (unsigned)nfo.read_regs[3],
             (unsigned)nfo.read_regs[4]);
    simplelist_addline(
             "   %s: %02X %02X %02X %02X %02X", "Write",
             (unsigned)nfo.write_regs[0], (unsigned)nfo.write_regs[1],
             (unsigned)nfo.write_regs[2], (unsigned)nfo.write_regs[3],
             (unsigned)nfo.write_regs[4]);
#endif /* TEA5767 */
#if (CONFIG_TUNER & SI4700)
    IF_TUNER_TYPE(SI4700)
    {
        struct si4700_dbg_info nfo;
        si4700_dbg_info(&nfo);
        simplelist_setline("SI4700 regs:");
        for (int i = 0; i < 16; i += 4) {
            simplelist_addline("%02X: %04X %04X %04X %04X",
                i, nfo.regs[i], nfo.regs[i+1], nfo.regs[i+2], nfo.regs[i+3]);
        }
    }
#endif /* SI4700 */
#if (CONFIG_TUNER & RDA5802)
    IF_TUNER_TYPE(RDA5802)
    {
        struct rda5802_dbg_info nfo;
        rda5802_dbg_info(&nfo);
        simplelist_setline("RDA5802 regs:");
        for (int i = 0; i < 16; i += 4) {
            simplelist_addline("%02X: %04X %04X %04X %04X",
                i, nfo.regs[i], nfo.regs[i+1], nfo.regs[i+2], nfo.regs[i+3]);
        }
    }
#endif /* RDA55802 */
#if (CONFIG_TUNER & STFM1000)
    IF_TUNER_TYPE(STFM1000)
    {
        struct stfm1000_dbg_info nfo;
        stfm1000_dbg_info(&nfo);
        simplelist_setline("STFM1000 regs:");
        simplelist_addline("chipid: 0x%lx", nfo.chipid);
    }
#endif /* STFM1000 */
#if (CONFIG_TUNER & TEA5760)
    IF_TUNER_TYPE(TEA5760)
    {
        struct tea5760_dbg_info nfo;
        tea5760_dbg_info(&nfo);
        simplelist_setline("TEA5760 regs:");
        for (int i = 0; i < 16; i += 4) {
            simplelist_addline("%02X: %02X %02X %02X %02X",
                i, nfo.read_regs[i], nfo.read_regs[i+1], nfo.read_regs[i+2], nfo.read_regs[i+3]);
        }
    }
#endif /* TEA5760 */

#ifdef HAVE_RDS_CAP
    {
        char buf[65*4];
        uint16_t pi;
        time_t seconds;

        tuner_get_rds_info(RADIO_RDS_NAME, buf, sizeof (buf));
        tuner_get_rds_info(RADIO_RDS_PROGRAM_INFO, &pi, sizeof (pi));
        simplelist_addline("PI:%04X PS:'%-8s'", pi, buf);
        tuner_get_rds_info(RADIO_RDS_TEXT, buf, sizeof (buf));
        simplelist_addline("RT:%s", buf);
        tuner_get_rds_info(RADIO_RDS_CURRENT_TIME, &seconds, sizeof (seconds));

        struct tm* time = gmtime(&seconds);
        simplelist_addline(
            "CT:%4d-%02d-%02d %02d:%02d:%02d",
            time->tm_year + 1900, time->tm_mon + 1, time->tm_mday,
            time->tm_hour, time->tm_min, time->tm_sec);
    }
#endif /* HAVE_RDS_CAP */
    return ACTION_REDRAW;
}
static bool dbg_fm_radio(void)
{
    struct simplelist_info info;
#ifdef CONFIG_TUNER_MULTI
    tuner_type = tuner_detect_type();
#endif
    info.scroll_all = true;
    simplelist_info_init(&info, "FM Radio", 0, NULL);
    simplelist_reset_lines();
    simplelist_setline("HW detected: no");

    info.action_callback = radio_hardware_present()?radio_callback : NULL;
    return simplelist_show_list(&info);
}
#endif /* CONFIG_TUNER */
#endif /* !SIMULATOR */

#if !defined(APPLICATION)
extern bool do_screendump_instead_of_usb;

static bool dbg_screendump(void)
{
    do_screendump_instead_of_usb = !do_screendump_instead_of_usb;
    splashf(HZ, "Screendump %sabled", do_screendump_instead_of_usb?"en":"dis");
    return false;
}
#endif /* !APPLICATION */

extern bool write_metadata_log;

static bool dbg_metadatalog(void)
{
    write_metadata_log = !write_metadata_log;
    splashf(HZ, "Metadata log %sabled", write_metadata_log ? "en" : "dis");
    return false;
}

#if defined(CPU_COLDFIRE)
static bool dbg_set_memory_guard(void)
{
    static const struct opt_items names[MAXMEMGUARD] = {
        { "None",             -1 },
        { "Flash ROM writes", -1 },
        { "Zero area (all)",  -1 }
    };
    int mode = system_memory_guard(MEMGUARD_KEEP);

    set_option( "Catch mem accesses", &mode, RB_INT, names, MAXMEMGUARD, NULL);
    system_memory_guard(mode);

    return false;
}
#endif /* defined(CPU_COLDFIRE) */

#if defined(HAVE_EEPROM) && !defined(HAVE_EEPROM_SETTINGS)
static bool dbg_write_eeprom(void)
{
    int fd = open("/internal_eeprom.bin", O_RDONLY);

    if (fd >= 0)
    {
        char buf[EEPROM_SIZE];
        int rc = read(fd, buf, EEPROM_SIZE);

        if(rc == EEPROM_SIZE)
        {
            int old_irq_level = disable_irq_save();

            int err = eeprom_24cxx_write(0, buf, sizeof buf);
            if (err)
                splashf(HZ*3, "Eeprom write failure (%d)", err);
            else
                splash(HZ*3, "Eeprom written successfully");

            restore_irq(old_irq_level);
        }
        else
        {
            splashf(HZ*3, "File read error (%d)",rc);
        }
        close(fd);
    }
    else
    {
        splash(HZ*3, "Failed to open 'internal_eeprom.bin'");
    }

    return false;
}
#endif /* defined(HAVE_EEPROM) && !defined(HAVE_EEPROM_SETTINGS) */
#ifdef CPU_BOOST_LOGGING
static bool cpu_boost_log(void)
{
    int count = cpu_boost_log_getcount();
    char *str = cpu_boost_log_getlog_first();
    bool done;
    lcd_setfont(FONT_SYSFIXED);
    for (int i = 0; i < count ;)
    {
        lcd_clear_display();
        for(int j=0; j<LCD_HEIGHT/SYSFONT_HEIGHT; j++,i++)
        {
            if (!str)
                str = cpu_boost_log_getlog_next();
            if (str)
            {
                if(strlen(str) > LCD_WIDTH/SYSFONT_WIDTH)
                    lcd_puts_scroll(0, j, str);
                else
                    lcd_puts(0, j,str);
            }
            str = NULL;
        }
        lcd_update();
        done = false;
        while (!done)
        {
            switch(get_action(CONTEXT_STD,TIMEOUT_BLOCK))
            {
                case ACTION_STD_OK:
                case ACTION_STD_PREV:
                case ACTION_STD_NEXT:
                    done = true;
                break;
                case ACTION_STD_CANCEL:
                    i = count;
                    done = true;
                break;
            }
        }
    }
    lcd_scroll_stop();
    get_action(CONTEXT_STD,TIMEOUT_BLOCK);
    lcd_setfont(FONT_UI);
    return false;
}

static bool cpu_boost_log_dump(void)
{
    int fd;
    int count = cpu_boost_log_getcount();
    char *str = cpu_boost_log_getlog_first();

    splashf(HZ, "Boost Log File Dumped");

    /* nothing to print ? */
    if(count == 0)
        return false;

#if CONFIG_RTC
    char fname[MAX_PATH];
    struct tm *nowtm = get_time();
    fd = open_pathfmt(fname, sizeof(fname), O_CREAT|O_WRONLY|O_TRUNC,
                      "%s/boostlog_%04d%02d%02d%02d%02d%02d.txt", ROCKBOX_DIR,
                      nowtm->tm_year + 1900, nowtm->tm_mon + 1, nowtm->tm_mday,
                      nowtm->tm_hour, nowtm->tm_min, nowtm->tm_sec);
#else
    fd = open(ROCKBOX_DIR "/boostlog.txt", O_CREAT|O_WRONLY|O_TRUNC, 0666);
#endif
    if(-1 != fd) {
        for (int i = 0; i < count; i++)
        {
            if (!str)
                str = cpu_boost_log_getlog_next();
            if (str)
            {
               fdprintf(fd, "%s\n", str);
               str = NULL;
            }
        }

        close(fd);
        return true;
    }

    return false;
}
#endif

#if (defined(HAVE_WHEEL_ACCELERATION) && (CONFIG_KEYPAD==IPOD_4G_PAD) \
     && !defined(IPOD_MINI) && !defined(SIMULATOR))
extern bool wheel_is_touched;
extern int old_wheel_value;
extern int new_wheel_value;
extern int wheel_delta;
extern unsigned int accumulated_wheel_delta;
extern unsigned int wheel_velocity;

static bool dbg_scrollwheel(void)
{
    lcd_setfont(FONT_SYSFIXED);

    while (1)
    {
        if (action_userabort(HZ/10))
            break;

        lcd_clear_display();

        /* show internal variables of scrollwheel driver */
        lcd_putsf(0, 0, "wheel touched: %s", (wheel_is_touched) ? "true" : "false");
        lcd_putsf(0, 1, "new position: %2d", new_wheel_value);
        lcd_putsf(0, 2, "old position: %2d", old_wheel_value);
        lcd_putsf(0, 3, "wheel delta: %2d", wheel_delta);
        lcd_putsf(0, 4, "accumulated delta: %2d", accumulated_wheel_delta);
        lcd_putsf(0, 5, "velo [deg/s]: %4d", (int)wheel_velocity);

        /* show effective accelerated scrollspeed */
        lcd_putsf(0, 6, "accel. speed: %4d",
                button_apply_acceleration((1<<31)|(1<<24)|wheel_velocity) );

        lcd_update();
    }
    lcd_setfont(FONT_UI);
    return false;
}
#endif

static bool dbg_talk(void)
{
    struct simplelist_info list;
    struct talk_debug_data data;
    talk_get_debug_data(&data);

    simplelist_info_init(&list, "Voice Information:", 0, NULL);

    list.scroll_all = true;
    list.timeout = HZ;

    simplelist_reset_lines();

    simplelist_setline("Current voice file:");
    if (data.status != TALK_STATUS_ERR_NOFILE)
        simplelist_addline(" %s", data.voicefile);
    else
        simplelist_setline(" No voice information available");

    if (data.status != TALK_STATUS_OK)
    {
        simplelist_addline("Talk Status: ERR (%i)",
                    data.status);
        return simplelist_show_list(&list);
    }
    else
        simplelist_setline("Talk Status: OK");
    simplelist_setline("Number of (empty) clips in voice file:");
    simplelist_addline(" (%d) %d", data.num_empty_clips, data.num_clips);
    simplelist_setline("Min/Avg/Max size of clips:");
    simplelist_addline(" %d / %d / %d",
                    data.min_clipsize, data.avg_clipsize, data.max_clipsize);
    simplelist_setline("Memory allocated:");
    simplelist_addline(" %ld.%02ld KB",
                    data.memory_allocated / 1024, data.memory_allocated % 1024);
    simplelist_addline("Memory used:");
    simplelist_addline(" %ld.%02ld KB",
                       data.memory_used / 1024, data.memory_used % 1024);
    simplelist_setline("Number of clips in cache:");
    simplelist_addline(" %d", data.cached_clips);
    simplelist_setline("Cache hits / misses:");
    simplelist_addline("%d / %d", data.cache_hits, data.cache_misses);

    return simplelist_show_list(&list);
}

#ifdef HAVE_USBSTACK
#if (defined(ROCKBOX_HAS_LOGF) && defined(USB_ENABLE_SERIAL))
#ifdef USB_ENABLE_SERIAL
static bool toggle_usb_serial(void)
{
    bool enabled = !usb_get_serial();

    usb_set_serial(enabled);
    splashf(HZ, "USB Serial %s", enabled ? "enabled" : "disabled");

    return false;
}
#endif /* USB_ENABLE_SERIAL */
#endif

#ifdef USB_ENABLE_AUDIO
static int dbg_usb_audio_cb(int action, struct gui_synclist *lists)
{
    (void)lists;
    simplelist_reset_lines();
    simplelist_addline("%sabled", usb_core_driver_enabled(USB_DRIVER_AUDIO)?"En":"Dis");
    simplelist_addline("%sPlaying", usb_audio_get_playing()?"":"Not ");
    simplelist_addline("iface: %d alt: %d", usb_audio_get_main_intf(), usb_audio_get_alt_intf());
    simplelist_addline("out ep: 0x%X in ep: 0x%X", usb_audio_get_out_ep(), usb_audio_get_in_ep());
    simplelist_addline("Volume: %d", usb_audio_get_cur_volume());
    simplelist_addline("Playback Frequency: %lu", usb_audio_get_playback_sampling_frequency());
    simplelist_addline("Frames dropped: %d", usb_audio_get_frames_dropped());
    simplelist_addline("Buffers filled: %f", (double)usb_audio_get_prebuffering_avg()/(1<<16)); // convert from 16.16 fixed to float
    simplelist_addline("Min: %d / Max: %d", usb_audio_get_prebuffering_maxmin(false), usb_audio_get_prebuffering_maxmin(true));
    simplelist_addline("Samples used per Frame: %f", (double)usb_audio_get_samplesperframe()/(1<<16)); // convert from 16.16 fixed to float
    simplelist_addline("Samples received per frame: %f", (double)usb_audio_get_samples_rx_perframe()/(1<<16)); // convert from 16.16 fixed to float
    simplelist_addline("Samples diff: %f", (double)(usb_audio_get_samplesperframe()-usb_audio_get_samples_rx_perframe())/(1<<16)); // convert from 16.16 fixed to float
    simplelist_addline("%s", usb_audio_get_underflow()?"UNDERFLOW!":" ");
    simplelist_addline("%s", usb_audio_get_overflow()?"OVERFLOW!":" ");
    simplelist_addline("%s", usb_audio_get_alloc_failed()?"ALLOC FAILED!":" ");
    if (action == ACTION_NONE)
    {
        action = ACTION_REDRAW;
    }
    return action;
}
static bool dbg_usb_audio(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "USB Audio", 0, NULL);
    info.scroll_all = true;
    info.action_callback = dbg_usb_audio_cb;
    return simplelist_show_list(&info);
}
#endif /* USB_ENABLE_AUDIO */
#endif /* HAVE_USBSTACK */

#if CONFIG_USBOTG == USBOTG_ISP1583
extern int dbg_usb_num_items(void);
extern const char* dbg_usb_item(int selected_item, void *data,
                                char *buffer, size_t buffer_len);

static int isp1583_action_callback(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_NONE)
        action = ACTION_REDRAW;
    return action;
}

static bool dbg_isp1583(void)
{
    struct simplelist_info isp1583;
    isp1583.scroll_all = true;
    simplelist_info_init(&isp1583, "ISP1583", dbg_usb_num_items(), NULL);
    isp1583.timeout = HZ/100;
    isp1583.get_name = dbg_usb_item;
    isp1583.action_callback = isp1583_action_callback;
    return simplelist_show_list(&isp1583);
}
#endif

#if defined(HAVE_BOOTDATA) && !defined(SIMULATOR)
static bool dbg_boot_data(void)
{
    struct simplelist_info info;
    info.scroll_all = true;
    simplelist_info_init(&info, "Boot data", 0, NULL);
    simplelist_reset_lines();

    if (!boot_data_valid)
    {
        simplelist_setline("Boot data invalid");
        simplelist_addline("Magic[0]: %08lx", boot_data.magic[0]);
        simplelist_addline("Magic[1]: %08lx", boot_data.magic[1]);
        simplelist_addline("Length: %lu", boot_data.length);
    }
    else
    {
        simplelist_setline("Boot data valid");
        simplelist_addline("Version: %d", (int)boot_data.version);

        if (boot_data.version == 0)
        {
            simplelist_addline("Boot volume: %d", (int)boot_data._boot_volume);
        }
        else if (boot_data.version == 1)
        {
            simplelist_addline("Boot drive: %d", (int)boot_data.boot_drive);
            simplelist_addline("Boot partition: %d", (int)boot_data.boot_partition);
        }
        simplelist_setline("Boot path:");
        simplelist_addline(" %s%s/%s", root_realpath(), BOOTDIR, BOOTFILE);
    }

    simplelist_setline("Bootdata RAW:");
    for (size_t i = 0; i < boot_data.length; i += 4)
    {
        simplelist_addline("%02x: %02x %02x %02x %02x", i,
                           boot_data.payload[i + 0], boot_data.payload[i + 1],
                           boot_data.payload[i + 2], boot_data.payload[i + 3]);
    }

    return simplelist_show_list(&info);
}
#endif /* defined(HAVE_BOOTDATA) && !defined(SIMULATOR) */

#if defined(HAVE_DEVICEDATA)// && !defined(SIMULATOR)
static bool dbg_device_data(void)
{
    struct simplelist_info info;
    info.scroll_all = true;
    simplelist_info_init(&info, "Device data", 0, NULL);
    simplelist_reset_lines();

    simplelist_setline("Device data");

#if defined(EROS_QN)
    simplelist_addline("Hardware Revision: %d", (int)device_data.hw_rev);
#endif
    simplelist_addline("Struct Ver: %d", (int)device_data.version);

    simplelist_setline("Device data RAW:");
    for (size_t i = 0; i < device_data.length; i += 4)
    {
        simplelist_addline("%02zx: %02x %02x %02x %02x", i,
                           device_data.payload[i + 0], device_data.payload[i + 1],
                           device_data.payload[i + 2], device_data.payload[i + 3]);
    }

    return simplelist_show_list(&info);
}
#endif /* defined(HAVE_DEVICEDATA)*/


#if defined(IPOD_6G) && !defined(SIMULATOR)
static bool dbg_syscfg(void) {
    struct simplelist_info info;
    struct SysCfg syscfg;

    simplelist_info_init(&info, "SysCfg NOR contents", 0, NULL);
    simplelist_reset_lines();

    const ssize_t result = syscfg_read(&syscfg);

    if (result == -1) {
        simplelist_setline("SCfg magic not found");
        return simplelist_show_list(&info);
    }

    simplelist_addline("Total size: %lu bytes, %lu entries", syscfg.header.size, syscfg.header.num_entries);

    if (result > 0) {
        simplelist_addline("Wrong size: expected %ld, got %lu", result, syscfg.header.size);
        return simplelist_show_list(&info);
    }

    if (syscfg.header.num_entries > SYSCFG_MAX_ENTRIES) {
        simplelist_addline("Too many entries, showing only first %u", SYSCFG_MAX_ENTRIES);
    }

    const size_t syscfg_num_entries = MIN(syscfg.header.num_entries, SYSCFG_MAX_ENTRIES);

    for (size_t i = 0; i < syscfg_num_entries; i++) {
        const struct SysCfgEntry* entry = &syscfg.entries[i];
        const char* tag = (char *)&entry->tag;
        const uint32_t* data32 = (uint32_t *)entry->data;

        switch (entry->tag) {
            case SYSCFG_TAG_SRNM:
                simplelist_addline("Serial number (SrNm): %s", entry->data);
                break;
            case SYSCFG_TAG_FWID:
                simplelist_addline("Firmware ID (FwId): %07lX", data32[1] & 0x0FFFFFFF);
                break;
            case SYSCFG_TAG_HWID:
                simplelist_addline("Hardware ID (HwId): %08lX", data32[0]);
                break;
            case SYSCFG_TAG_HWVR:
                simplelist_addline("Hardware version (HwVr): %06lX", data32[1]);
                break;
            case SYSCFG_TAG_CODC:
                simplelist_addline("Codec (Codc): %s", entry->data);
                break;
            case SYSCFG_TAG_SWVR:
                simplelist_addline("Software version (SwVr): %s", entry->data);
                break;
            case SYSCFG_TAG_MLBN:
                simplelist_addline("Logic board serial number (MLBN): %s", entry->data);
                break;
            case SYSCFG_TAG_MODN:
                simplelist_addline("Model number (Mod#): %s", entry->data);
                break;
            case SYSCFG_TAG_REGN:
                simplelist_addline("Sales region (Regn): %08lX %08lX", data32[0], data32[1]);
                break;
            default:
                simplelist_addline("%c%c%c%c: %08lX %08lX %08lX %08lX",
                    tag[3], tag[2], tag[1], tag[0],
                    data32[0], data32[1], data32[2], data32[3]
                );
                break;
        }
    }

    return simplelist_show_list(&info);
}

#define FLASH_PAGES (FLASH_SIZE >> 12)
#define FLASH_PAGE_SIZE (FLASH_SIZE >> 8)

static bool dbg_bootflash_dump(void) {
    splashf(HZ, "Please wait...");

    int fd;

    fd = creat("/bootflash.bin", 0666);

    if (fd < 0)
    {
        splashf(HZ * 3, "Error opening file");
        return false;
    }

    uint8_t page[FLASH_PAGE_SIZE];
    bootflash_init(SPI_PORT);

    for (int i = 0; i < FLASH_PAGES; i++) {
        bootflash_read(SPI_PORT, i << 12, FLASH_PAGE_SIZE, page);
        write(fd, page, FLASH_PAGE_SIZE);
    }

    bootflash_close(SPI_PORT);
    close(fd);
    splashf(HZ * 3, "Dump saved to /bootflash.bin");

    return false;
}
#endif

#if defined(HIBY_LINUX) && !defined(SIMULATOR)
static bool view_ram_info(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "RAM Info", 0, NULL);

    simplelist_reset_lines();
    simplelist_addline("Rockbox: %d MB", MEMORYSIZE);

    struct sysinfo sys_info;
    if (sysinfo(&sys_info) == 0) {
        long total_ram = sys_info.totalram * sys_info.mem_unit / 1024 / 1024;
        long free_ram = sys_info.freeram * sys_info.mem_unit / 1024 / 1024;
        long buffer_ram = sys_info.bufferram * sys_info.mem_unit / 1024 / 1024;
        simplelist_addline("Total RAM: %ld MB", total_ram);
        simplelist_addline("Free RAM: %ld MB", free_ram);
        simplelist_addline("Buffer RAM: %ld MB", buffer_ram);

        /* Try to read MemAvailable from /proc/meminfo for a better "free" estimate */
        FILE *fp = fopen("/proc/meminfo", "r");
        if (fp) {
            char line[128];
            long mem_avail = -1;
            long cached = -1;
            while (fgets(line, sizeof(line), fp)) {
                if (sscanf(line, "MemAvailable: %ld kB", &mem_avail) == 1) {
                        mem_avail /= 1024;
                }
                else if (sscanf(line, "Cached: %ld kB", &cached) == 1) {
                        cached /= 1024;
                }
            }
            fclose(fp);

            if (mem_avail != -1) {
                /* Estimate of how much memory is available for starting new applications */
                simplelist_addline("Available: %ld MB", mem_avail);
            }
            if (cached != -1) {
                /* Memory used by the page cache. */
                simplelist_addline("Cached: %ld MB", cached);
            }
        }
    }

    return simplelist_show_list(&info);
}
#endif

#ifdef IPOD_VIDEO
/* Bring-up test for the VideoCore host interface (bcm_host.c).  Needs the files extracted from the
   iPod firmware (vc_extract.py) under /.rockbox/vc/Resources/VideoCore/{Boot,Library}/.
   Writes a log to /bcm_host_log.txt (flushed line by line). */
#include "bcm_host.h"
extern void bcm_io_lock(void);
extern void bcm_io_unlock(void);
extern void bcm_io_read(unsigned vc_addr, void *dst, unsigned len);
extern void bcm_io_write(unsigned vc_addr, const void *src, unsigned len);
extern int  bcm_use_vmcs_image(const void *img, unsigned len);
extern int bcm_restore_rom_vmcs(void);           /* v47, lcd-video.c */
extern int  bcm_io_dead(void);        /* v16: host bus wait timed out -> the VC stopped answering */
extern void bcm_io_doorbell(void);    /* v23: BCM_CONTROL = 0x31, the wake/notify write */
extern void bcm_io_status(unsigned v[4]);   /* v24: bus status registers */
extern void bcm_lcd_freeze(bool on);        /* v26: no more legacy LCD traffic to the BCM */
extern unsigned bcm_get_boot_info(unsigned snap[4]);

#define BCM_VC_ROOT    "/.rockbox/vc"
#define BCM_VMCS_PATH  BCM_VC_ROOT "/Resources/VideoCore/Boot/vmcs.bin"
#define BCM_VC_FLAT    "/.rockbox/VideoCore"    /* v56: everything in one folder: vmcs.bin, mplayer.vll, h264dec.vll, aacdec.vll, ... */
#define BCM_LOG_PATH   "/bcm_host_log.txt"
#include "playlist.h"     /* v51: the video's sound is played by Rockbox's playback engine */
#include "sound.h"
#include "pcmbuf.h"
#include "pcm_mixer.h"    /* v53: the codec's sample rate for the VideoCore's sound */
#include "wmcodec.h"
#include "wm8758.h"
#include "backlight-target.h"   /* v56: _backlight_hw_enable() -- dark before the panel is switched off */
#define BCM_TESTFILE_PATH BCM_VC_ROOT "/../../testfile.txt"   /* -> /.rockbox/testfile.txt: first line = video path */

static unsigned char bcm_vmcs_buf[204800] __attribute__((aligned(4))); /* vmcs.bin is 201376 B */
static int bcm_log_fd = -1;
static bool bcm_vc_flat;               /* v56: the VideoCore files are in /.rockbox/VideoCore/ */
/* v56: the log file is overwritten in place (no truncate + new clusters: on the 2 TB FAT32 volume v55 spent 0.8 s before the
   second log line) and cut to its length when it is closed */
static void bcm_log_open(void)
{
    bcm_log_fd = open(BCM_LOG_PATH, O_WRONLY);
    if (bcm_log_fd < 0) bcm_log_fd = creat(BCM_LOG_PATH, 0666);
}
static void bcm_log_close(void)
{
    if (bcm_log_fd < 0) return;
    ftruncate(bcm_log_fd, lseek(bcm_log_fd, 0, SEEK_CUR));
    close(bcm_log_fd);
    bcm_log_fd = -1;
}
static const char *bcm_play_path;   /* v47: set when started from the file browser ("player mode") */
static bool bcm_player_mode;

static long bcm_t_start;
static char bcm_last_line[72];
static long bcm_last_tick;
/* v25: software watchdog. The v24 log stops right after "vmcs.bin ... bytes", i.e. inside the VC boot, and the iPod sat
   there. lcd-video.c's own bus waits have no timeout, so a hang there never returns. A tick task (it runs even when the
   main thread spins) reboots the iPod if no log line has been written for BCM_WD_SECS; the log written so far stays. */
#define BCM_WD_SECS 45
static volatile long bcm_wd_last;
static volatile int  bcm_wd_on;
static long bcm_last_sync;
static long bcm_sync_every;      /* v45: while a video plays, fsync at most this often (ticks; 0 = old rules) */
static int  bcm_trace_42;        /* v45: number of PDS 0x42 trace lines written */
static void bcm_wd_tick(void)
{
    if (bcm_wd_on && (long)(current_tick - bcm_wd_last) > BCM_WD_SECS * HZ)
        system_reboot();
}
static void bcm_logline(const char *s, int n)
{
    char b[200], t[160];
    long e;
    int m;
    if (bcm_log_fd < 0 || n <= 0)
        return;
    if (n > 159) n = 159;
    memcpy(t, s, n); t[n] = 0;
    e = current_tick - bcm_t_start;
    m = snprintf(b, sizeof b, "[%3ld.%02ld] %s\n", e / HZ, e % HZ, t);   /* v16: seconds since the test started */
    write(bcm_log_fd, b, m);
    /* v25: fsync is slow on this 2 TB FAT32 volume right after a boot (0.3-1 s per line in the v24 log). Lines that start with
       a space (VCFS traces, dumps, list items) are flushed at most twice a second; everything else is flushed at once. */
    /* v45: the v44 log shows one PDS 0x42 every ~33 ms; an fsync on this volume costs 0.1-0.6 s, so during playback
       the log is flushed only every log_sync seconds (lines are still written at once). */
    if (bcm_sync_every ? (long)(current_tick - bcm_last_sync) >= bcm_sync_every
                       : (t[0] != ' ' || (long)(current_tick - bcm_last_sync) >= HZ / 2))
    {
        fsync(bcm_log_fd);             /* keep the log even if the VideoCore hangs and we must hard-reset */
        bcm_last_sync = current_tick;
    }
    bcm_wd_last = current_tick;
    memcpy(bcm_last_line, t, sizeof bcm_last_line - 1); bcm_last_line[sizeof bcm_last_line - 1] = 0;
    bcm_last_tick = current_tick;
}
#define BLOG(...) do { char _b[160]; int _n = snprintf(_b, sizeof _b, __VA_ARGS__); \
                       bcm_logline(_b, _n); } while (0)

static bool bcm_dead_seen;
static bool bcm_check_dead(void)
{
    int d = bcm_io_dead();
    char last[72]; long ms;
    if (!d) return false;
    if (!bcm_dead_seen)
    {
        bcm_dead_seen = true;
        memcpy(last, bcm_last_line, sizeof last);
        ms = (current_tick - bcm_last_tick) * 1000 / HZ;
        BLOG("VC BUS DEAD (code %d: 1 write-ready, 2 read-addr, 3 read-ready): no answer %ld ms after the last log line:", d, ms);
        BLOG("  last line was: %s", last);
    }
    return true;
}
static int cfg_quiet_ref(void);
static void bcm_trace_cb(const char *tag, uint32_t a, uint32_t b, const char *s)
{
    if (a == 0x42 && !strcmp(tag, "pds op") && ++bcm_trace_42 > 24 && cfg_quiet_ref())
        return;                                    /* v45: one line per frame request cost feed time; the first 24 are kept */
    BLOG("  trace: %s %08x %08x %.60s", tag, (unsigned)a, (unsigned)b, s ? s : "");
}

/* Send one command and wait, logging a heartbeat every ~2s instead of going silent -- so a log
   that stops mid-wait means the DEVICE stopped responding right then (a freeze), not just that
   this particular command is slow. Never resends: one bcm_gencmd_start(), repeated
   bcm_gencmd_poll() until it settles or max_secs elapses. */
static int bcm_gencmd_watched(const char *cmd, char *resp, size_t resp_sz, int max_secs)
{
    BLOG("sending: %s", cmd);
    if (bcm_gencmd_start(cmd) < 0) { BLOG("  (not attached)"); return -1; }
    long t0 = current_tick, last_beat = t0, last_act = t0;
    uint32_t rx0 = bcm_host_get_stats()->rx_msgs;
    int r = -3;
    while (r == -3) {
        r = bcm_gencmd_poll(resp, resp_sz);
        if (bcm_check_dead()) { r = -5; break; }
        if (r != -3) break;
        if (current_tick - last_beat >= 2 * HZ) {
            BLOG("  ...still waiting (%lds)", (current_tick - t0) / HZ);
            last_beat = current_tick;
        }
        if (current_tick - t0 >= max_secs * HZ) { r = -2; break; }
        /* v45: was sleep(HZ/20): while a command was pending the frame feed (serviced inside the poll) dropped to 20/s.
           Most replies arrive within 100 ms, so poll without sleeping for that long, then once per tick. */
        /* v56: ...and keep polling without sleeping while the VC is sending requests (VCFS reads of the .vll files: a 10 ms
           sleep per read made loading mplayer + H264dec + AACdec take ~2.5 s) */
        if (bcm_host_get_stats()->rx_msgs != rx0) { rx0 = bcm_host_get_stats()->rx_msgs; last_act = current_tick; }
        if (current_tick - t0 < HZ / 10 || current_tick - last_act < HZ / 10) yield(); else sleep(1);
    }
    return r;
}

static long bcm_cmd_t0;
static void bcm_log_text(const char *cmd, int r, const char *text)
{
    long dt = current_tick - bcm_cmd_t0;
    BLOG("%s -> %d (%ld.%02lds):", cmd, r, dt / HZ, (dt % HZ) * 100 / HZ);
    while (*text)
    {
        char seg[124];
        int n = 0;
        while (*text && *text != '\n' && n < 120)
            seg[n++] = *text++;
        if (*text == '\n')
            text++;
        seg[n] = 0;
        if (n)
            BLOG("  %s", seg);
    }
}

/* Minimal PDS callbacks for the v8 playback probe: log what the VC asks for, feed nothing real
   yet (bcm_pds_send_empty() on every get_frame). The point of this run is only to see whether
   mp_selectplay/mp_play produce ANY VideoCore-initiated PDS traffic, and with what parameters --
   not to play the file. */
static int pds_probe_starts, pds_probe_frames, pds_probe_seeks, pds_probe_stops;
static void pds_probe_start(int id, uint32_t w1)
{
    pds_probe_starts++;
    BLOG("  PDS start: stream=%d w1=%08x", id, (unsigned)w1);
}
static void pds_probe_get_frame(int id, uint32_t w1, uint32_t w2)
{
    pds_probe_frames++;
    BLOG("  PDS get_frame #%d: stream=%d w1=%08x w2=%08x", pds_probe_frames, id, (unsigned)w1, (unsigned)w2);
    bcm_pds_send_empty();          /* "no frame available" -- we are not feeding real data yet */
}
static void pds_probe_seek(int id, uint32_t pos)
{
    pds_probe_seeks++;
    BLOG("  PDS seek: stream=%d pos=%u", id, (unsigned)pos);
}
static void pds_probe_stop(void)
{
    pds_probe_stops++;
    BLOG("  PDS stop");
}
static const struct bcm_pds_ops pds_probe_ops = {
    pds_probe_start, pds_probe_get_frame, pds_probe_seek, pds_probe_stop
};

/* v8 playback probe: read /.rockbox/testfile.txt for a video path (so the file can be changed
   without a rebuild), then try mp_region/mp_selectplay/mp_play and watch for PDS traffic.
   "rgb565" and "yuv422i" are real mode strings found in this vmcs.bin (0x1ccd80-ish); nothing
   else about the mp_region/mp_selectplay argument syntax has been confirmed on hardware yet, so
   this logs whatever gencmd replies come back, error or not. */

/* ---- v13: minimal streaming MP4 (H.264) reader -------------------------------------------------
   Nothing is loaded into RAM except what one frame needs: the sample/chunk/time tables are read
   from the file on demand, so a 1-hour clip with 100k+ samples is fine. Output is H.264 *Annex B*
   (00 00 00 01 start codes, SPS+PPS in front of the first frame and every IDR) because
   h264dec.vll exports the JM-style GetAnnexbNALU/InterpretSPS/InterpretPPS -- i.e. it parses an
   Annex-B byte stream, not MP4's length-prefixed samples. The PDS frame payload being raw Annex B
   is therefore an inference from those symbols, not something seen on hardware yet. */
static int cfg_frame_avcc = 1;     /* v27: 1 = send MP4 samples unchanged (AVCC, length-prefixed) like the stock firmware; 0 = convert to Annex B (v13-v26) */
#define M4_RAW_MAX 61440
#define M4_OUT_MAX 65536
#define FOURCC(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
static uint8_t m4_raw[M4_RAW_MAX];
static uint8_t m4_out[M4_OUT_MAX];
static struct {
    int fd;
    uint32_t timescale, width, height;
    uint8_t  profile, level, nal_len;
    uint8_t  ps[512]; uint32_t ps_len;                  /* SPS+PPS, Annex B */
    uint8_t  avcc[128]; uint32_t avcc_len;              /* raw avcC record (v22: candidate "extradata" for the 0x60 block) */
    uint32_t stts, stts_n, stsc, stsc_n, stsz, stsz_fixed, stsz_n, stco, stco_n, stss, stss_n;   /* v42: stss */
    bool     need_ps;                                   /* v42: prepend SPS/PPS to the next sample (after a seek) */
    bool     co64;
    uint32_t idx, chunk, s_in_chunk, off_in_chunk, stsc_i, spc, next_first;   /* sample cursor */
    uint32_t stts_i, stts_left, stts_delta; uint64_t dts;
} m4 = { .fd = -1 };
/* v53: the AAC track (stock route, see ma_trak) */
static struct {
    bool ok;
    uint32_t timescale, channels, rate, bitrate;
    uint8_t asc[16]; uint32_t asc_len;
    uint32_t stts, stts_n, stsc, stsc_n, stsz, stsz_fixed, stsz_n, stco, stco_n; bool co64;
    uint32_t idx, chunk, s_in_chunk, off_in_chunk, stsc_i, spc, next_first, stts_i, stts_left, stts_delta; uint64_t dts;
} ma;

static uint32_t m4_be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static bool m4_rd(uint32_t off, void *buf, uint32_t n)
{
    if (lseek(m4.fd, (off_t)off, SEEK_SET) != (off_t)off) return false;
    return read(m4.fd, buf, n) == (ssize_t)n;
}
/* v45: table and data caches.  Up to v44 every frame did three lseek+read pairs at unrelated file offsets (the
   stsz entry and the stco entry in the moov box, then the sample itself in mdat).  Rockbox seeks backwards by walking
   the FAT cluster chain from the start of the file, so each frame cost several disk reads; the v44 log shows the VC
   asking for a frame every ~33 ms (= real time), i.e. we never got ahead and mplayer's ~380 KB start/seek pre-buffer
   took 9-13 s to fill.  Now: table entries come from six 4 KB windows (LRU), sample data from a 64 KB read-ahead
   window, so a frame normally needs no disk access at all. */
static int cfg_m4cache_ref(void);
/* v59 read-ahead for solid-state storage (iFlash etc.; helps a hard disk too).  Up to v58 the player read the file in
   64 KB pieces about once a second and the sample tables in 4 KB pieces, so the drive never got to sleep and every piece
   paid a wake-up.  Now (one buffer from Rockbox's allocator, freed when the video ends):
   - the whole 'moov' box (every sample table) is read into RAM once when the file is opened (up to cfg moov_kb);
   - the sample data goes through a ring of cfg ra_kb (4 MB = ~1 min of a 500 kbit/s video): it is refilled in 64 KB
     steps between frame requests whenever less than a quarter of it is still ahead, until it is full, and then the
     drive is told to sleep at once (an SSD wakes from standby in milliseconds) -- one short burst a minute;
   - a seek / FF / REW outside the ring restarts it at the new place with a small read. */
static int cfg_ra_kb = 4096, cfg_moov_kb = -1, cfg_ra_sleep = 1;   /* v79: moov_kb -1 = from the memory profile */
/* v79: memory profiles -- one build runs on both (the RAM size is probed at boot: probed_ramsize).
   ring = average bit rate x seconds, between lo and hi MB, per Video Buffering mode (low-power, balanced, performance);
   moov = the sample tables kept whole in RAM up to this size (no disk reads for seeks / FF: the v76 FF log had 138 table
   reads from disk); reserve = left free after the ring for subtitles, fonts and the VideoCore's helpers;
   font = a subtitle font smaller than this is loaded whole (CJK fonts are 1-2 MB). */
struct bcm_memprof { const char *name; uint16_t secs[3]; uint8_t lo_mb[3], hi_mb[3]; uint16_t moov_kb, reserve_kb, font_kb; };
static const struct bcm_memprof bcm_mp64 = { "64 MB", { 900, 180, 300 }, { 8, 4, 16 }, { 40, 16, 40 }, 16384, 6144, 2048 };
static const struct bcm_memprof bcm_mp32 = { "32 MB", { 240, 60, 120 }, { 4, 2, 6 }, { 12, 6, 12 }, 4096, 3072, 400 };
static const struct bcm_memprof *bcm_memprof(void) { return probed_ramsize == 64 ? &bcm_mp64 : &bcm_mp32; }
/* v74: Settings > Video Buffering picks the ring and the refill point (cfg ra_kb > 0 / ra_mode still override):
     Low-power    ring = ~3 min of this video (4..24 MB), refilled when 1/8 is left, the SSD sleeps at once after:
                  the fewest drive wake-ups (one ~every 2.5 min)
     Balanced     ring = ~1 min of this video (2..8 MB), refilled at 1/4 (the v59 behaviour, now sized by bit rate)
     Performance  ring = ~2 min (8..24 MB), refilled at 1/2, the drive is left to Rockbox's own timer: the most
                  data ready for seeks / FF / REW and a busy card */
static int cfg_ra_mode = -1;          /* -1 = the setting */
static int ra_mode, ra_refill_div = 4;
static bool ra_sleep_on = true;
static int ra_handle = -1;
static uint8_t *ra_buf;               /* ring: cfg_ra_kb KB */
static uint32_t ra_size;
static uint32_t ra_lo, ra_hi;         /* file bytes [ra_lo, ra_hi) are in the ring at offset % ra_size */
static uint32_t ra_fsize;
static uint32_t ra_pos[2];            /* last data offsets read: video, audio */
static int ra_stream;                 /* which of the two the next m4_rdd is for */
static bool ra_filling, ra_slept;
static uint32_t ra_bursts, ra_misses, ra_read_kb;
static uint8_t *mv_buf;               /* the moov box */
static uint32_t mv_s, mv_e;
static void ra_free(void)
{
    if (ra_handle > 0) { core_unpin(ra_handle); core_free(ra_handle); }
    ra_handle = -1; ra_buf = NULL; mv_buf = NULL; ra_size = 0; ra_lo = ra_hi = 0;
}
static bool ra_read_at(uint32_t off, uint32_t n)      /* file [off, off+n) -> ring; off == ra_hi */
{
    while (n)
    {
        uint32_t p = off % ra_size, c = ra_size - p;
        ssize_t r;
        if (c > n) c = n;
        if (lseek(m4.fd, (off_t)off, SEEK_SET) != (off_t)off) return false;
        r = read(m4.fd, ra_buf + p, c);
        if (r <= 0) return false;
        off += (uint32_t)r; n -= (uint32_t)r; ra_hi = off; ra_read_kb += (uint32_t)r / 1024;
        if ((uint32_t)r < c) return n == 0;
    }
    return true;
}
/* restart the ring at off (seek / first read) */
static bool ra_restart(uint32_t off, uint32_t need)
{
    uint32_t n = 65536;
    if (need + 1024 > n) n = need + 1024;
    if (n > ra_size) n = ra_size;
    ra_lo = ra_hi = off & ~511u;
    if (ra_hi + n > ra_fsize) n = ra_fsize - ra_hi;
    ra_misses++;
    ra_filling = true;
    return ra_read_at(ra_hi, n);
}
static bool ra_get(uint32_t off, void *buf, uint32_t n)
{
    uint8_t *o = buf;
    if (!(off >= ra_lo && off + n <= ra_hi))
    {
        if (!ra_restart(off, n)) return false;
        if (!(off >= ra_lo && off + n <= ra_hi)) return false;
    }
    ra_pos[ra_stream & 1] = off;
    while (n)
    {
        uint32_t p = off % ra_size, c = ra_size - p;
        if (c > n) c = n;
        memcpy(o, ra_buf + p, c); o += c; off += c; n -= c;
    }
    return true;
}
/* called from the playback loop between frame requests */
static void ra_tick(void)
{
    uint32_t keep, lead, room, n, ahead;
    int k, inside = 0;
    if (!ra_buf || m4.fd < 0 || ra_hi >= ra_fsize) return;
    keep = ra_hi; lead = ra_lo;
    for (k = 0; k < 2; k++)                                                   /* only positions inside the ring count */
        if (ra_pos[k] >= ra_lo && ra_pos[k] < ra_hi)
        {
            inside++;
            if (ra_pos[k] < keep) keep = ra_pos[k];                           /* the stream that is furthest behind */
            if (ra_pos[k] > lead) lead = ra_pos[k];
        }
    if (!inside) return;
    keep = keep > ra_lo + 131072 ? keep - 131072 : ra_lo;                     /* keep 128 KB behind it */
    ra_lo = keep;
    ahead = ra_hi - lead;
    if (!ra_filling && ahead < ra_size / ra_refill_div) { ra_filling = true; ra_slept = false; ra_bursts++; }
    if (!ra_filling) return;
    room = ra_size - (ra_hi - ra_lo);
    n = room < 65536 ? room : 65536;
    if (ra_hi + n > ra_fsize) n = ra_fsize - ra_hi;
    if (n < 4096 || !ra_read_at(ra_hi, n) || ra_hi >= ra_fsize || room - n < 65536)
    {
        ra_filling = false;
        /* full: an SSD is put to sleep at once (wakes in ms); a hard disk keeps Rockbox's own spin-down timer */
        if (cfg_ra_sleep && ra_sleep_on && !ra_slept && ata_disk_isssd()) { storage_sleepnow(); ra_slept = true; }
    }
}
#define M4C_WIN 4096
#define M4C_N   6
static struct { uint32_t base, len, use; uint8_t d[M4C_WIN]; } m4c[M4C_N];
static uint32_t m4c_clock, m4c_miss, m4d_miss;
static uint8_t m4d[65536];
static uint32_t m4d_base, m4d_len;
static void m4c_reset(void) { int i; for (i = 0; i < M4C_N; i++) { m4c[i].len = 0; m4c[i].use = 0; } m4d_len = 0; }
static bool m4_rdc(uint32_t off, void *buf, uint32_t n)
{
    uint8_t *o = buf;
    if (mv_buf && off >= mv_s && off + n <= mv_e) { memcpy(buf, mv_buf + (off - mv_s), n); return true; }   /* v59 */
    if (!cfg_m4cache_ref()) return m4_rd(off, buf, n);
    while (n)
    {
        uint32_t base = off & ~(uint32_t)(M4C_WIN - 1), k, c;
        int i, best = 0;
        for (i = 0; i < M4C_N; i++) if (m4c[i].len && m4c[i].base == base) break;
        if (i == M4C_N)
        {
            ssize_t r;
            for (i = 1; i < M4C_N; i++) if (m4c[i].use < m4c[best].use) best = i;
            i = best; m4c[i].len = 0; m4c_miss++;
            if (lseek(m4.fd, (off_t)base, SEEK_SET) != (off_t)base) return false;
            r = read(m4.fd, m4c[i].d, M4C_WIN);
            if (r <= 0) return false;
            m4c[i].base = base; m4c[i].len = (uint32_t)r;
        }
        m4c[i].use = ++m4c_clock;
        k = off - base;
        if (k >= m4c[i].len) return false;
        c = m4c[i].len - k; if (c > n) c = n;
        memcpy(o, m4c[i].d + k, c); o += c; off += c; n -= c;
    }
    return true;
}
/* sample data: served from the read-ahead window when it is inside it, else the window is refilled from there */
static bool m4_rdd(uint32_t off, void *buf, uint32_t n)
{
    if (ra_buf && n <= ra_size / 2)
    {   /* v59 -- unless the sound lies far from the picture in the file (not interleaved): then it uses the small window */
        uint32_t d = ra_stream == 1 && ra_pos[0] ? (off > ra_pos[0] ? off - ra_pos[0] : ra_pos[0] - off) : 0;
        if (d < ra_size / 2) return ra_get(off, buf, n);
    }
    if (!cfg_m4cache_ref() || n > sizeof m4d / 2) return m4_rd(off, buf, n);
    if (!(m4d_len && off >= m4d_base && off + n <= m4d_base + m4d_len))
    {
        uint32_t base = off & ~511u; ssize_t r;
        m4d_len = 0; m4d_miss++;
        if (lseek(m4.fd, (off_t)base, SEEK_SET) != (off_t)base) return false;
        r = read(m4.fd, m4d, sizeof m4d);
        if (r <= 0) return false;
        m4d_base = base; m4d_len = (uint32_t)r;
        if (off + n > m4d_base + m4d_len) return false;
    }
    memcpy(buf, m4d + (off - m4d_base), n);
    return true;
}
static uint32_t m4_u32(uint32_t off) { uint8_t b[4]; return m4_rdc(off, b, 4) ? m4_be32(b) : 0; }
static uint32_t m4_u16(uint32_t off) { uint8_t b[2]; return m4_rdc(off, b, 2) ? (uint32_t)((b[0]<<8)|b[1]) : 0; }
static uint32_t m4_u8(uint32_t off)  { uint8_t b[1]; return m4_rdc(off, b, 1) ? b[0] : 0; }

/* iterate the boxes in [*pos,end): payload range returned via ps and pe; false when none left or malformed */
static bool m4_box(uint32_t *pos, uint32_t end, uint32_t *type, uint32_t *ps, uint32_t *pe)
{
    uint8_t h[8]; uint32_t size, hdr = 8;
    if (*pos + 8 > end || !m4_rd(*pos, h, 8)) return false;
    size = m4_be32(h); *type = m4_be32(h + 4);
    if (size == 1) { uint8_t e[8]; if (!m4_rd(*pos + 8, e, 8) || m4_be32(e)) return false; size = m4_be32(e + 4); hdr = 16; }
    else if (size == 0) size = end - *pos;
    if (size < hdr || size > end - *pos) return false;
    *ps = *pos + hdr; *pe = *pos + size; *pos += size;
    return true;
}
static bool m4_find(uint32_t s, uint32_t e, uint32_t want, uint32_t *cs, uint32_t *ce)
{
    uint32_t pos = s, t, a, b;
    while (m4_box(&pos, e, &t, &a, &b)) if (t == want) { *cs = a; *ce = b; return true; }
    return false;
}

static bool m4_trak(uint32_t ps, uint32_t pe)
{
    uint32_t ms, me, a, b, minf_s, minf_e, stbl_s, stbl_e, pos, t, s, e;
    bool have_avc = false, have[5] = { false, false, false, false, false };
    if (!m4_find(ps, pe, FOURCC('m','d','i','a'), &ms, &me)) return false;
    if (!m4_find(ms, me, FOURCC('h','d','l','r'), &a, &b) || m4_u32(a + 8) != FOURCC('v','i','d','e')) return false;
    if (!m4_find(ms, me, FOURCC('m','d','h','d'), &a, &b)) return false;
    m4.timescale = m4_u8(a) == 1 ? m4_u32(a + 20) : m4_u32(a + 12);
    if (!m4.timescale) return false;
    if (!m4_find(ms, me, FOURCC('m','i','n','f'), &minf_s, &minf_e)) return false;
    if (!m4_find(minf_s, minf_e, FOURCC('s','t','b','l'), &stbl_s, &stbl_e)) return false;
    m4.co64 = false;
    pos = stbl_s;
    while (m4_box(&pos, stbl_e, &t, &s, &e))
    {
        if (t == FOURCC('s','t','s','d'))
        {
            uint32_t ent = s + 8, esz = m4_u32(ent), cs, ce;
            if (m4_u32(ent + 4) != FOURCC('a','v','c','1') || esz < 86 + 8) return false;
            m4.width = m4_u16(ent + 32); m4.height = m4_u16(ent + 34);
            if (!m4_find(ent + 86, ent + esz, FOURCC('a','v','c','C'), &cs, &ce) || ce - cs < 7) return false;
            m4.avcc_len = (ce - cs) <= sizeof m4.avcc ? (ce - cs) : 0;
            if (m4.avcc_len && !m4_rd(cs, m4.avcc, m4.avcc_len)) m4.avcc_len = 0;
            m4.profile = (uint8_t)m4_u8(cs + 1); m4.level = (uint8_t)m4_u8(cs + 3);
            m4.nal_len = (uint8_t)((m4_u8(cs + 4) & 3) + 1);
            {   uint32_t p = cs + 6, i, n = m4_u8(cs + 5) & 0x1f; int pass;
                m4.ps_len = 0;
                for (pass = 0; pass < 2; pass++)
                {
                    if (pass == 1) { n = m4_u8(p); p++; }            /* PPS count */
                    for (i = 0; i < n; i++)
                    {
                        uint32_t l = m4_u16(p); p += 2;
                        if (m4.ps_len + 4 + l > sizeof m4.ps || p + l > ce) return false;
                        m4.ps[m4.ps_len++] = 0; m4.ps[m4.ps_len++] = 0; m4.ps[m4.ps_len++] = 0; m4.ps[m4.ps_len++] = 1;
                        if (!m4_rd(p, m4.ps + m4.ps_len, l)) return false;
                        m4.ps_len += l; p += l;
                    }
                }
            }
            have_avc = true;
        }
        else if (t == FOURCC('s','t','t','s')) { m4.stts = s + 8; m4.stts_n = m4_u32(s + 4); have[0] = true; }
        else if (t == FOURCC('s','t','s','c')) { m4.stsc = s + 8; m4.stsc_n = m4_u32(s + 4); have[1] = true; }
        else if (t == FOURCC('s','t','s','z')) { m4.stsz_fixed = m4_u32(s + 4); m4.stsz_n = m4_u32(s + 8); m4.stsz = s + 12; have[2] = true; }
        else if (t == FOURCC('s','t','s','s')) { m4.stss = s + 8; m4.stss_n = m4_u32(s + 4); }
        else if (t == FOURCC('s','t','c','o')) { m4.stco = s + 8; m4.stco_n = m4_u32(s + 4); m4.co64 = false; have[3] = true; }
        else if (t == FOURCC('c','o','6','4')) { m4.stco = s + 8; m4.stco_n = m4_u32(s + 4); m4.co64 = true;  have[3] = true; }
    }
    have[4] = have_avc;
    return have[0] && have[1] && have[2] && have[3] && have[4] && m4.stsz_n && m4.stts_n && m4.stsc_n && m4.stco_n;
}

static void m4_rewind(void)
{
    m4.idx = 0; m4.chunk = 1; m4.s_in_chunk = 0; m4.off_in_chunk = 0; m4.stsc_i = 0;
    m4.spc = m4_u32(m4.stsc + 4); if (!m4.spc) m4.spc = 1;
    m4.next_first = m4.stsc_n > 1 ? m4_u32(m4.stsc + 12) : 0xffffffffu;
    m4.stts_i = 0; m4.stts_left = m4_u32(m4.stts); m4.stts_delta = m4_u32(m4.stts + 4); m4.dts = 0;
}

static void m4_close(void) { if (m4.fd >= 0) close(m4.fd); m4.fd = -1; m4c_reset(); ra_free(); }

/* v42: random access, the way the stock player does it (ARM positions its demuxer on a key frame, then sends
   "mp_seek <ms>"; the VC flushes and answers with PDS 0x43 {session, ms}; the stock 0x43 handler (rt 0x20b304)
   only resets the per-stream timestamps and resumes feeding from the demuxer's new position).
   m4_key_at(ms): sample index (0-based) of the last sync sample at or before ms, and its time.
   m4_seek_sample(k): put the cursor on sample k without reading the samples before it. */
static uint64_t m4_dts_of(uint32_t k)
{
    uint64_t t = 0; uint32_t i, base = 0;
    for (i = 0; i < m4.stts_n; i++)
    {
        uint32_t c = m4_u32(m4.stts + 8 * i), dl = m4_u32(m4.stts + 8 * i + 4);
        if (k < base + c) return t + (uint64_t)(k - base) * dl;
        t += (uint64_t)c * dl; base += c;
    }
    return t;
}
static uint32_t m4_sample_at_ms(uint32_t ms)
{
    uint64_t want = (uint64_t)ms * m4.timescale / 1000, t = 0; uint32_t i, base = 0;
    for (i = 0; i < m4.stts_n; i++)
    {
        uint32_t c = m4_u32(m4.stts + 8 * i), dl = m4_u32(m4.stts + 8 * i + 4);
        /* v45: nearest sample, not the one before: key-frame times are reported in whole ms (rounded down), and a
           seek to such a time must land on that key frame, not on the one before it (FF scrubbing got stuck) */
        if (dl && want < t + (uint64_t)c * dl)
        {
            uint32_t r = base + (uint32_t)((want - t + dl / 2) / dl);
            return m4.stsz_n && r >= m4.stsz_n ? m4.stsz_n - 1 : r;
        }
        t += (uint64_t)c * dl; base += c;
    }
    return m4.stsz_n ? m4.stsz_n - 1 : 0;
}
static uint32_t m4_key_at(uint32_t ms, uint32_t *key_ms)
{
    uint32_t s = m4_sample_at_ms(ms), k = s;
    if (m4.stss && m4.stss_n)
    {
        uint32_t lo = 0, hi = m4.stss_n;              /* largest sync sample number (1-based) <= s+1 */
        while (hi - lo > 1) { uint32_t mid = (lo + hi) / 2; if (m4_u32(m4.stss + 4 * mid) <= s + 1) lo = mid; else hi = mid; }
        k = m4_u32(m4.stss + 4 * lo); k = k ? k - 1 : 0;
    }
    if (key_ms) *key_ms = (uint32_t)(m4_dts_of(k) * 1000ULL / m4.timescale);
    return k;
}
static void m4_seek_sample(uint32_t k)
{
    uint32_t i, chunk = 1, first_sample = 0, spc, nf, j;
    if (k >= m4.stsz_n) k = m4.stsz_n ? m4.stsz_n - 1 : 0;
    m4_rewind();
    for (i = 0; i < m4.stsc_n; i++)                   /* find the stsc run and chunk that hold sample k */
    {
        uint32_t fc = m4_u32(m4.stsc + 12 * i);
        spc = m4_u32(m4.stsc + 12 * i + 4); if (!spc) spc = 1;
        nf = (i + 1 < m4.stsc_n) ? m4_u32(m4.stsc + 12 * (i + 1)) : 0xffffffffu;
        if (nf != 0xffffffffu && k >= first_sample + (nf - fc) * spc) { first_sample += (nf - fc) * spc; continue; }
        chunk = fc + (k - first_sample) / spc;
        m4.stsc_i = i; m4.spc = spc; m4.next_first = nf;
        m4.chunk = chunk; m4.s_in_chunk = (k - first_sample) % spc; m4.off_in_chunk = 0;
        for (j = k - m4.s_in_chunk; j < k; j++)
            m4.off_in_chunk += m4.stsz_fixed ? m4.stsz_fixed : m4_u32(m4.stsz + 4 * j);
        break;
    }
    m4.idx = k;
    {   uint64_t t = 0; uint32_t base = 0;            /* stts cursor */
        for (i = 0; i < m4.stts_n; i++)
        {
            uint32_t c = m4_u32(m4.stts + 8 * i), dl = m4_u32(m4.stts + 8 * i + 4);
            if (k < base + c) { m4.stts_i = i; m4.stts_left = base + c - k; m4.stts_delta = dl; m4.dts = t + (uint64_t)(k - base) * dl; break; }
            t += (uint64_t)c * dl; base += c;
        }
    }
    m4.need_ps = true;
}
static uint32_t m4_dur_ms, m4_br;            /* v20: duration (ms) and average bit rate (bit/s) of the whole file */
static uint32_t m4_moov_s, m4_moov_e;        /* v73: the moov payload (the subtitle track scan) */

static bool ma_trak(uint32_t ps, uint32_t pe);   /* v53: audio track (below) */
static void ma_rewind(void);
/* 0 = ok, <0 = reason */
static int mp4_open(const char *path)
{
    uint32_t fsz, pos = 0, t, ps, pe, moov_s = 0, moov_e = 0;
    off_t sz;
    m4_close();
    m4.fd = open(path, O_RDONLY);
    if (m4.fd < 0) return -1;
    sz = lseek(m4.fd, 0, SEEK_END);
    if (sz <= 0) { m4_close(); return -2; }
    fsz = (uint32_t)sz;
    while (m4_box(&pos, fsz, &t, &ps, &pe)) if (t == FOURCC('m','o','o','v')) { moov_s = ps; moov_e = pe; break; }
    if (!moov_e) { m4_close(); return -3; }
    m4_moov_s = moov_s; m4_moov_e = moov_e;
    {   /* v59: one buffer: the moov box + the read-ahead ring */
        uint32_t mlen = moov_e - moov_s + 8, want;
        long t0 = current_tick;
        ra_fsize = fsz;
        uint32_t ring = 0, br = 0;
        const struct bcm_memprof *mp = bcm_memprof();
        uint32_t reserve = (uint32_t)mp->reserve_kb * 1024;
        if (mlen > (uint32_t)(cfg_moov_kb >= 0 ? cfg_moov_kb : mp->moov_kb) * 1024) mlen = 0;
        {   /* v74: the ring from the buffering setting and this file's average bit rate (mvhd duration); v79: per RAM size */
            const uint16_t *secs = mp->secs;
            const uint8_t *lo_mb = mp->lo_mb, *hi_mb = mp->hi_mb;
            static const int div[3] = { 8, 4, 2 };
            uint32_t a, b, ts, dur;
            ra_mode = cfg_ra_mode >= 0 ? cfg_ra_mode : global_settings.bcm_video_buffer;
            if (ra_mode < 0 || ra_mode > 2) ra_mode = 1;
            {   /* v78: the longest of the movie header and the tracks' own durations.  v74 used the mvhd alone, and some
                   encoders write a short one there: "134740 kbit/s" for a 330 kbit/s film, so every mode asked for its
                   largest ring (24 MB in Performance) -- with subtitles on a 32 MB iPod that left too little memory. */
                uint64_t best_ms = 0;
                uint32_t tpos = moov_s, tt, tps, tpe, ma2, mb2, ms2, me2;
                if (m4_find(moov_s, moov_e, FOURCC('m','v','h','d'), &a, &b))
                {
                    bool v1 = m4_u8(a) == 1;
                    ts = m4_u32(a + (v1 ? 20 : 12)); dur = m4_u32(a + (v1 ? 28 : 16));   /* v1: low word of the u64 */
                    if (ts && dur) best_ms = (uint64_t)dur * 1000 / ts;
                }
                while (m4_box(&tpos, moov_e, &tt, &tps, &tpe))
                    if (tt == FOURCC('t','r','a','k') && m4_find(tps, tpe, FOURCC('m','d','i','a'), &ms2, &me2)
                        && m4_find(ms2, me2, FOURCC('m','d','h','d'), &ma2, &mb2))
                    {
                        bool v1 = m4_u8(ma2) == 1;
                        uint32_t t2 = m4_u32(ma2 + (v1 ? 20 : 12)), d2 = m4_u32(ma2 + (v1 ? 28 : 16));
                        if (t2 && d2 && (uint64_t)d2 * 1000 / t2 > best_ms) best_ms = (uint64_t)d2 * 1000 / t2;
                    }
                if (best_ms >= 1000) br = (uint32_t)((uint64_t)fsz * 8000ULL / best_ms);
                if (br > 20000000) br = 0;                     /* still nonsense: treat as unknown */
            }
            ring = br ? (uint32_t)((uint64_t)br / 8 * secs[ra_mode]) : 4u << 20;
            if (ring < (uint32_t)lo_mb[ra_mode] << 20) ring = (uint32_t)lo_mb[ra_mode] << 20;
            if (ring > (uint32_t)hi_mb[ra_mode] << 20) ring = (uint32_t)hi_mb[ra_mode] << 20;

            if (cfg_ra_kb > 0 && cfg_ra_kb != 4096) ring = (uint32_t)cfg_ra_kb * 1024;     /* an explicit ra_kb wins */
            if (cfg_ra_kb <= 0) ring = 0;
            if (ring > fsz + 65536) ring = (fsz + 65536 + 65535) & ~65535u;               /* a short clip: all of it */
            ra_refill_div = div[ra_mode];
            ra_sleep_on = ra_mode != 2;
        }
        want = mlen + ring;
        /* v79: ask for the ring + the reserve, then give the reserve back: whatever comes next (subtitles, fonts) still
           finds that much free, however full memory was.  Smaller rings when even that does not fit. */
        while (want && (ra_handle = core_alloc_ex(want + reserve, NULL)) <= 0 && ring > (1u << 20))
        {   /* not that much free memory: a smaller ring */
            ring = (ring / 2) & ~65535u; want = mlen + ring;
        }
        if (want && ra_handle <= 0) ra_handle = core_alloc_ex(want, NULL);   /* no room for the reserve: the ring anyway */
        else if (want && ra_handle > 0) core_shrink(ra_handle, core_get_data(ra_handle), want);
        if (want && ra_handle > 0)
        {
            uint8_t *p;
            core_pin(ra_handle);
            p = core_get_data(ra_handle);
            if (mlen)
            {
                mv_s = moov_s - 8; mv_e = mv_s + mlen;
                if (lseek(m4.fd, (off_t)mv_s, SEEK_SET) == (off_t)mv_s && read(m4.fd, p, mlen) == (ssize_t)mlen) mv_buf = p;
                else mlen = 0;
            }
            if (ring >= 65536) { ra_buf = p + ((mlen + 31) & ~31u); ra_size = ring & ~65535u; }
            ra_lo = ra_hi = 0; ra_pos[0] = ra_pos[1] = 0; ra_filling = false; ra_bursts = ra_misses = ra_read_kb = 0;
            BLOG("read-ahead: %s profile, moov %u KB in RAM (%ld ms), ring %u KB%s -- %s, %u kbit/s, refill at 1/%d%s", mp->name,
                 mv_buf ? mlen / 1024 : 0,
                 (current_tick - t0) * 1000 / HZ, ra_size / 1024, ata_disk_isssd() ? " (solid-state drive)" : "",
                 ra_mode == 0 ? "low-power" : ra_mode == 2 ? "performance" : "balanced", (unsigned)(br / 1000), ra_refill_div,
                 ra_sleep_on ? "" : ", no forced sleep");
        }
        else BLOG("read-ahead: no buffer (%u KB wanted) -- the v45 small caches are used", want / 1024);
    }
    memset(&ma, 0, sizeof ma);                   /* v53: the AAC track too, if there is one */
    pos = moov_s;
    while (m4_box(&pos, moov_e, &t, &ps, &pe))
        if (t == FOURCC('t','r','a','k') && !ma.ok && ma_trak(ps, pe)) { ma.ok = true; ma_rewind(); break; }
    if (!ma.ok) memset(&ma, 0, sizeof ma);
    pos = moov_s;
    while (m4_box(&pos, moov_e, &t, &ps, &pe))
        if (t == FOURCC('t','r','a','k') && m4_trak(ps, pe))
        {
            uint64_t ticks = 0; uint32_t i;
            for (i = 0; i < m4.stts_n && i < 100000; i++)
                ticks += (uint64_t)m4_u32(m4.stts + 8 * i) * m4_u32(m4.stts + 8 * i + 4);
            m4_dur_ms = (uint32_t)(ticks * 1000ULL / m4.timescale);
            m4_br = m4_dur_ms ? (uint32_t)((uint64_t)fsz * 8000ULL / m4_dur_ms) : 0;
            m4_rewind();
            return 0;
        }
    m4_close();
    return -4;                                   /* no usable H.264 (avc1) video track */
}

/* Next video sample as Annex B in m4_out. Returns its length, 0 at end of stream, -1 for a sample
   that was skipped (too big / unreadable -- cursor still advances so the caller can just retry). */
static int m4_next(uint32_t *ts_ms, bool *key)
{
    uint32_t size, coff, off, p, o = 0, n, k;
    bool idr = false, first = (m4.idx == 0);
    if (m4.fd < 0 || m4.idx >= m4.stsz_n || m4.chunk > m4.stco_n) return 0;
    size = m4.stsz_fixed ? m4.stsz_fixed : m4_u32(m4.stsz + 4 * m4.idx);
    coff = m4.co64 ? m4_u32(m4.stco + 8 * (m4.chunk - 1) + 4) : m4_u32(m4.stco + 4 * (m4.chunk - 1));
    off  = coff + m4.off_in_chunk;
    *ts_ms = (uint32_t)(m4.dts * 1000ULL / m4.timescale);

    m4.idx++; m4.off_in_chunk += size;                                   /* advance the cursors first */
    if (++m4.s_in_chunk >= m4.spc)
    {
        m4.chunk++; m4.s_in_chunk = 0; m4.off_in_chunk = 0;
        if (m4.chunk >= m4.next_first && m4.stsc_i + 1 < m4.stsc_n)
        {
            m4.stsc_i++; m4.spc = m4_u32(m4.stsc + 12 * m4.stsc_i + 4); if (!m4.spc) m4.spc = 1;
            m4.next_first = m4.stsc_i + 1 < m4.stsc_n ? m4_u32(m4.stsc + 12 * (m4.stsc_i + 1)) : 0xffffffffu;
        }
    }
    m4.dts += m4.stts_delta;
    if (m4.stts_left && --m4.stts_left == 0 && m4.stts_i + 1 < m4.stts_n)
    {
        m4.stts_i++; m4.stts_left = m4_u32(m4.stts + 8 * m4.stts_i); m4.stts_delta = m4_u32(m4.stts + 8 * m4.stts_i + 4);
    }

    *key = false;
    ra_stream = 0;
    if (size == 0 || size > M4_RAW_MAX || !m4_rdd(off, m4_raw, size)) return -1;
    for (p = 0; p + m4.nal_len <= size; )                               /* pass 1: is there an IDR? */
    {
        n = 0; for (k = 0; k < m4.nal_len; k++) n = (n << 8) | m4_raw[p + k];
        if (n == 0 || p + m4.nal_len + n > size) break;
        if ((m4_raw[p + m4.nal_len] & 0x1f) == 5) idr = true;
        p += m4.nal_len + n;
    }
    if (cfg_frame_avcc)                                                  /* v27: stock-style AVCC sample, no conversion */
    {
        memcpy(m4_out, m4_raw, size);
        *key = idr;
        return (int)size;
    }
    if ((first || idr || m4.need_ps) && m4.ps_len) { memcpy(m4_out, m4.ps, m4.ps_len); o = m4.ps_len; }
    m4.need_ps = false;
    for (p = 0; p + m4.nal_len <= size; )                               /* pass 2: length-prefixed -> Annex B */
    {
        n = 0; for (k = 0; k < m4.nal_len; k++) n = (n << 8) | m4_raw[p + k];
        if (n == 0 || p + m4.nal_len + n > size || o + 4 + n > M4_OUT_MAX) break;
        m4_out[o++] = 0; m4_out[o++] = 0; m4_out[o++] = 0; m4_out[o++] = 1;
        memcpy(m4_out + o, m4_raw + p + m4.nal_len, n); o += n;
        p += m4.nal_len + n;
    }
    *key = idr;
    return o ? (int)o : -1;
}


/* ---- v53: the AAC audio track, for the stock route (sound=2): the stock media layer gives the VC both streams --
   0x60 stream table with an audio block (type 0, 'AAC\0', bit rate, AudioSpecificConfig bytes, channels, sample rate:
   RAM snapshot of the stock media object, findings 1.2/3ar) and the get-frame handler (rt 0x20b39c) serves a request
   for stream -1 with whichever stream is behind.  Samples go out raw (no ADTS header), timestamps in ms. */
static uint8_t ma_buf[8192];
static uint32_t ma_desc_len(uint32_t *p)
{   /* MPEG-4 descriptor length: up to 4 bytes, 7 bits each */
    uint32_t n = 0; int i;
    for (i = 0; i < 4; i++) { uint32_t b = m4_u8((*p)++); n = (n << 7) | (b & 0x7f); if (!(b & 0x80)) break; }
    return n;
}
static bool ma_trak(uint32_t ps, uint32_t pe)
{
    uint32_t ms, me, a, b, minf_s, minf_e, stbl_s, stbl_e, pos, t, s, e;
    bool have_esds = false;
    if (!m4_find(ps, pe, FOURCC('m','d','i','a'), &ms, &me)) return false;
    if (!m4_find(ms, me, FOURCC('h','d','l','r'), &a, &b) || m4_u32(a + 8) != FOURCC('s','o','u','n')) return false;
    if (!m4_find(ms, me, FOURCC('m','d','h','d'), &a, &b)) return false;
    ma.timescale = m4_u8(a) == 1 ? m4_u32(a + 20) : m4_u32(a + 12);
    if (!ma.timescale) return false;
    if (!m4_find(ms, me, FOURCC('m','i','n','f'), &minf_s, &minf_e)) return false;
    if (!m4_find(minf_s, minf_e, FOURCC('s','t','b','l'), &stbl_s, &stbl_e)) return false;
    pos = stbl_s;
    while (m4_box(&pos, stbl_e, &t, &s, &e))
    {
        if (t == FOURCC('s','t','s','d'))
        {
            uint32_t ent = s + 8, esz = m4_u32(ent), cs, ce;
            if (m4_u32(ent + 4) != FOURCC('m','p','4','a') || esz < 36) return false;
            ma.channels = m4_u16(ent + 24);
            ma.rate = m4_u32(ent + 32) >> 16;
            if (m4_find(ent + 36, ent + esz, FOURCC('e','s','d','s'), &cs, &ce))
            {
                uint32_t p = cs + 4, l, end;                         /* skip version/flags */
                if (m4_u8(p++) == 0x03)                              /* ES_Descriptor */
                {
                    l = ma_desc_len(&p); end = p + l; (void)end;
                    {   uint32_t fl; p += 2; fl = m4_u8(p++);
                        if (fl & 0x80) p += 2;
                        if (fl & 0x40) p += 1 + m4_u8(p);
                        if (fl & 0x20) p += 2;
                    }
                    if (m4_u8(p++) == 0x04)                          /* DecoderConfigDescriptor */
                    {
                        l = ma_desc_len(&p);
                        ma.bitrate = m4_u32(p + 9);                  /* avgBitrate */
                        if (!ma.bitrate) ma.bitrate = m4_u32(p + 5);
                        p += 13;
                        if (m4_u8(p++) == 0x05)                      /* DecoderSpecificInfo = AudioSpecificConfig */
                        {
                            uint32_t k;
                            l = ma_desc_len(&p);
                            ma.asc_len = l < sizeof ma.asc ? l : sizeof ma.asc;
                            for (k = 0; k < ma.asc_len; k++) ma.asc[k] = (uint8_t)m4_u8(p + k);
                            have_esds = ma.asc_len >= 2;
                        }
                    }
                }
            }
        }
        else if (t == FOURCC('s','t','t','s')) { ma.stts = s + 8; ma.stts_n = m4_u32(s + 4); }
        else if (t == FOURCC('s','t','s','c')) { ma.stsc = s + 8; ma.stsc_n = m4_u32(s + 4); }
        else if (t == FOURCC('s','t','s','z')) { ma.stsz_fixed = m4_u32(s + 4); ma.stsz_n = m4_u32(s + 8); ma.stsz = s + 12; }
        else if (t == FOURCC('s','t','c','o')) { ma.stco = s + 8; ma.stco_n = m4_u32(s + 4); ma.co64 = false; }
        else if (t == FOURCC('c','o','6','4')) { ma.stco = s + 8; ma.stco_n = m4_u32(s + 4); ma.co64 = true; }
    }
    return have_esds && ma.stts_n && ma.stsc_n && ma.stsz_n && ma.stco_n;
}
static void ma_rewind(void)
{
    ma.idx = 0; ma.chunk = 1; ma.s_in_chunk = 0; ma.off_in_chunk = 0; ma.stsc_i = 0;
    ma.spc = m4_u32(ma.stsc + 4); if (!ma.spc) ma.spc = 1;
    ma.next_first = ma.stsc_n > 1 ? m4_u32(ma.stsc + 12) : 0xffffffffu;
    ma.stts_i = 0; ma.stts_left = m4_u32(ma.stts); ma.stts_delta = m4_u32(ma.stts + 4); ma.dts = 0;
}
/* put the cursor on the first sample at or after ms */
static void ma_seek_ms(uint32_t ms)
{
    uint64_t want = (uint64_t)ms * ma.timescale / 1000, tt = 0; uint32_t i, base = 0, k = 0, chunk, first_sample = 0, spc, nf, j;
    for (i = 0; i < ma.stts_n; i++)
    {
        uint32_t c = m4_u32(ma.stts + 8 * i), dl = m4_u32(ma.stts + 8 * i + 4);
        if (dl && want < tt + (uint64_t)c * dl) { k = base + (uint32_t)((want - tt + dl - 1) / dl); break; }
        tt += (uint64_t)c * dl; base += c; k = base;
    }
    if (k >= ma.stsz_n) k = ma.stsz_n ? ma.stsz_n - 1 : 0;
    ma_rewind();
    for (i = 0; i < ma.stsc_n; i++)
    {
        uint32_t fc = m4_u32(ma.stsc + 12 * i);
        spc = m4_u32(ma.stsc + 12 * i + 4); if (!spc) spc = 1;
        nf = (i + 1 < ma.stsc_n) ? m4_u32(ma.stsc + 12 * (i + 1)) : 0xffffffffu;
        if (nf != 0xffffffffu && k >= first_sample + (nf - fc) * spc) { first_sample += (nf - fc) * spc; continue; }
        chunk = fc + (k - first_sample) / spc;
        ma.stsc_i = i; ma.spc = spc; ma.next_first = nf;
        ma.chunk = chunk; ma.s_in_chunk = (k - first_sample) % spc; ma.off_in_chunk = 0;
        for (j = k - ma.s_in_chunk; j < k; j++) ma.off_in_chunk += ma.stsz_fixed ? ma.stsz_fixed : m4_u32(ma.stsz + 4 * j);
        break;
    }
    ma.idx = k;
    {   uint64_t t2 = 0; uint32_t b2 = 0;
        for (i = 0; i < ma.stts_n; i++)
        {
            uint32_t c = m4_u32(ma.stts + 8 * i), dl = m4_u32(ma.stts + 8 * i + 4);
            if (k < b2 + c) { ma.stts_i = i; ma.stts_left = b2 + c - k; ma.stts_delta = dl; ma.dts = t2 + (uint64_t)(k - b2) * dl; break; }
            t2 += (uint64_t)c * dl; b2 += c;
        }
    }
}
static uint32_t ma_next_ts(void) { return ma.ok && ma.idx < ma.stsz_n ? (uint32_t)(ma.dts * 1000ULL / ma.timescale) : 0xffffffffu; }
static uint32_t m4_next_ts(void) { return m4.idx < m4.stsz_n ? (uint32_t)(m4.dts * 1000ULL / m4.timescale) : 0xffffffffu; }
/* next audio sample into ma_buf: length, 0 at the end, -1 skipped */
static int ma_next(uint32_t *ts_ms)
{
    uint32_t size, coff, off;
    if (!ma.ok || ma.idx >= ma.stsz_n || ma.chunk > ma.stco_n) return 0;
    size = ma.stsz_fixed ? ma.stsz_fixed : m4_u32(ma.stsz + 4 * ma.idx);
    coff = ma.co64 ? m4_u32(ma.stco + 8 * (ma.chunk - 1) + 4) : m4_u32(ma.stco + 4 * (ma.chunk - 1));
    off = coff + ma.off_in_chunk;
    *ts_ms = (uint32_t)(ma.dts * 1000ULL / ma.timescale);
    ma.idx++; ma.off_in_chunk += size;
    if (++ma.s_in_chunk >= ma.spc)
    {
        ma.chunk++; ma.s_in_chunk = 0; ma.off_in_chunk = 0;
        if (ma.chunk >= ma.next_first && ma.stsc_i + 1 < ma.stsc_n)
        {
            ma.stsc_i++; ma.spc = m4_u32(ma.stsc + 12 * ma.stsc_i + 4); if (!ma.spc) ma.spc = 1;
            ma.next_first = ma.stsc_i + 1 < ma.stsc_n ? m4_u32(ma.stsc + 12 * (ma.stsc_i + 1)) : 0xffffffffu;
        }
    }
    ma.dts += ma.stts_delta;
    if (ma.stts_left && --ma.stts_left == 0 && ma.stts_i + 1 < ma.stts_n)
    { ma.stts_i++; ma.stts_left = m4_u32(ma.stts + 8 * ma.stts_i); ma.stts_delta = m4_u32(ma.stts + 8 * ma.stts_i + 4); }
    ra_stream = 1;
    if (size == 0 || size > sizeof ma_buf || !m4_rdd(off, ma_buf, size)) return -1;
    return (int)size;
}

/* v14: the opcode-0x60 stream description the stock firmware sends before playback (rt 0x288f40, serialiser
   rt 0xc8770). 0x520 bytes = 328 words:
     w0  stream count            w1  [r4+0xc] (unknown; 0 here)     w2..w6  constants 1,0,1,1,0     w7 unused
     w8.. one block per stream:  type (0 audio / 1 video), codec fourcc ('H264' = 0x48323634, 'MPG4' =
          0x4d504734, 'AAC\0' = 0x414143, 'MP3\0' = 0x4d5033), word, word, N (= extradata bytes), N words
          (one byte each), word, word, byte, byte, key-bits, 32 words key, 16 words IV, word.
   KNOWN from disassembly: positions, type/codec words, the AES key/IV fields (zero = unencrypted).
   GUESSED: the "word" after the fourcc (I send 1000 = timescale, and f4 timestamps are in ms to match),
   and the two words after the extradata (I send width, height). All logged so a run can correct them. */
/* v16: optional /.rockbox/bcm_test.cfg, "key=value" per line, so the GUESSED fields can be varied without a
   rebuild.  si_w8   word after the fourcc in the 0x60 block: the stream BIT RATE in bit/s (default -1 = measured from the file;
                     mplayer.vll's property names for the handler info are type/bit_rate/height/width, duration, num_streams)
            si_dur  payload word 1 = duration in ms (default -1 = measured)      si_hw  1 = send height,width instead of width,height
            si_dim  1 = send width/height in the 0x60 block, 0 = zeros
            si_w1   payload word 1 of 0x60 ([this+0xc], unknown)      si_last  last block word ([s+0x58])
            si_count payload word 0 (stream count)                    f2  frame f2: 0, 1, or 2 = keyframe flag
            map     PDS buffer for the video stream (0 = A, stock; 1 = B)
            reply_ms delay before the 0x60 reply        skip_disp 1 = skip the display_control probe
            sel_wait seconds to wait for the mp_selectplay reply
            pm      1 = send "pm_set_policy min" like the stock firmware does before mp_selectplay / mp_play (default 1)
            step    1 = pause after each phase until you press a button (so you can say after which one the screen changed)
            cmds_end 1 = send the long `commands` list at the very end (tests the oversized-reply fix; default 1)
            region_when bitmask of when to send mp_region: 1 before mp_selectplay, 2 after it, 4 after mp_play (default 1; variants 1/2 use 7, variant 3 uses 0)
            region_mode 0 = mode=fill, 1 = mode=letterbox pixel="1 1"  (the only two strings in the stock firmware)
            region_w / region_h  destination size (default 320 x 240)
            variant N = a preset of the mp_region experiments below (0..3), or  variant=auto : use /.rockbox/bcm_variant.cnt, which
                     the test increments on every run (so just run the test again and again and send all the logs)
            audio    1 (default) = send "audio_enable 1" before mp_selectplay (the stock mp_selectplay path does: rt 0x1fa3dc -> 0x1f90f4)
            frame_avcc 1 (default) = hand the VC the MP4 samples unchanged (length-prefixed AVCC, as the stock does; the RAM snapshot
                     shows avcC extradata + "NAL length size - 1 = 3" in the 0x60 block); 0 = Annex B conversion (v13-v26)
            nolcd    1 (default) = after the VC boot freeze the legacy LCD driver (no splash markers, no lcd_tick kicks):
                     the v25 logs show VC[1F8] = 0xffff0000 (BCMCMD_LCD_UPDATE) again right before mp_selectplay, written
                     by Rockbox's own LCD code after marker B; the stock firmware never touches that word
            fix1f8   1 (default) = write 1 back into VC[0x1F8] after boot. The stock attach (rt 0x288058) requires VC[0x1F8]==1 and
                     never writes it; Rockbox's own first LCD update (lcd_awake) leaves 0xffff0000 there (the "now" log line)
            si_extra 0 none / 1 raw avcC record / 2 SPS+PPS (Annex B) as the "extradata" of the 0x60 stream block
            si_b20   the byte at [s+0x20] (stock always 1)    noreply 1 = do NOT answer the VC's 0x41 start    minimal 1 = 0x60 with zero streams
            reboot_end 1 = when the test is over, close the log and reboot the iPod (default 1): the new VC image has no
                     legacy LCD protocol, so Rockbox's own screen is dead afterwards and a second run in the same power
                     session booted the VC badly (v20 log)
            pm_play 1 = send pm_set_policy min before mp_play too (default 0: the second one hung the VC in v18)
            disp_all 1 = also try display_control 1 / model=1 (default 0: only "display_control 0 model=2 power=1")
            setvol   v28: 1 (default) = send "mp_setvol 0" right before audio_enable (stock rt 0x1f9158 does this at the start of
                     every mp_selectplay; mplayer.vll does not list the command, so the VC will probably answer "error")
            vchr47   v28: 0 (default) = answer VCHR op 0x47 with a zero-length payload like the stock (rt 0xfa0a8); 1 = the old 4 bytes
            flags_legacy v29: 1 = the pre-v29 chunk flag numbering (whole frame = 2); default 0 = stock (whole frame = 3)
            dc_seq v31: 0 = display_control power=1 only; 1 = power=-2 first, then 1; 2 = power=2 first, then 1
            lcdtest v33: 1 = draw colour bars into the VC LCD framebuffer and kick the legacy update mailbox; freeze 2 = freeze=0 before AND after display_control; dump_sdram (default 1): 4 MB snapshots of VC 0xC0000000; dump_sram (default 0)
            dump v32 (default 1 in variant 0): snapshots of VC 0..dump_len (default 0x140000) at dump_at1/dump_at2 seconds into playback -> /.rockbox/vcdump_play1.bin, _play2.bin
            vde v32: 1 = send vmcs_display_enable 1, 2 = vmcs_display_clear first
            probe_at v31: seconds into playback for the one-shot mp_region/mp_paint probe (default 30, 0 = off)
            freeze   v28: 1 = send "display_control 0 freeze=0" right after the VC boot (stock rt 0x164788 does after bring-up)
*/
static int cfg_si_w8 = -1, cfg_si_dur = -1, cfg_si_hw = 0, cfg_si_dim = 1, cfg_si_w1 = 0, cfg_si_last = -1, cfg_si_count = 1;
static int cfg_f2 = 2, cfg_map = 0, cfg_reply_ms = 0, cfg_skip_disp = 0, cfg_sel_wait = 12;
static int cfg_pm = 1, cfg_step = 0, cfg_cmds_end = 1, cfg_reboot_end = 1, cfg_audio = 1;
static int cfg_si_extra = 1, cfg_si_b20 = 1, cfg_noreply = 0, cfg_minimal = 0, cfg_variant = 0,   /* v43: the player config is the default, no cfg file needed */ cfg_fix1f8 = 1, cfg_nolcd = 1;
static int cfg_region_when = 1, cfg_region_mode = 0, cfg_region_w = 320, cfg_region_h = 240, cfg_pm_play = 0, cfg_disp_all = 0;
static int cfg_setvol = 1, cfg_vchr47 = 0, cfg_freeze = 0, cfg_flags_legacy = 0;
static int cfg_dc_seq = 0, cfg_probe_at = 0;
static int cfg_osd = 1, cfg_osd_layer = 100;                                         /* v44: playback overlay */
/* v73: subtitles: subs -1 = the setting, 0 off, 1 on; subs_layer 0 = just below the controls; subs_sync = ms between
   position reads while playing; subs_delay = ms added to the video position (+ = subtitles later) */
static int cfg_subs = -1, cfg_subs_layer = 0, cfg_subs_sync = 1500, cfg_subs_delay = 0;
static int cfg_scrub_mode = 0, cfg_scrub_tick = 300, cfg_step_ms = 40, cfg_tap_secs = 10, cfg_ff_rate = 0xa00;   /* v45: FF/REW */
static int cfg_log_sync = 10, cfg_seekmon = 12, cfg_quiet = 1, cfg_m4cache = 1;                                 /* v45: feed speed */
static int cfg_ff_accel = 1;                                                        /* v47 */
static int cfg_sound = 2, cfg_av_tol = 150, cfg_av_lead = 0, cfg_vol_start = 50, cfg_vol_step = 3;  /* v51 (v53: sound 2 = stock route) */
static int cfg_asc_n = 2, cfg_av_offset = 0, cfg_av_stall = 7;  /* v53 */
static int cfg_av_hz = 48000, cfg_av_mix = 1, cfg_vcvol = -1;
static int cfg_vol_direct = 1;
static int cfg_tv = -1, cfg_tv_enc = -1, cfg_tv_extra = 0, cfg_tv_osd = 1;
static int cfg_boost = 2;              /* v57: CPU boost 0 never, 1 always, 2 auto */
static bool av_boosted;
static long av_boost_until;
static void av_boost(bool on);   /* v55: force TV on/off, encoding number, 1 = add svideo=1 */
static int bcm_tv;                     /* v55: this video goes to the TV (display 2) */       /* v54: volume bar written straight into the visible window's image */  /* v53 stock route: codec rate, AUX into the output mixer, mp_setvol after mp_play */
static int v_sound;                  /* v53: route in use this run (cfg_sound, or 1 after a stall fallback) */
static bool v_fallback;              /* v53: the stock route stalled -> restart the video with Rockbox's sound */
static int cfg_av_ready = 8;                       /* v52: PCM chunks buffered before the sound counts as ready */
static int cfg_prep = 1;                                                            /* v78: start-up helper thread */
static int cfg_ff_resume = 1;                    /* v78: FF/REW release: 1 = resume where the VC is, 0 = seek to the shown frame */
static int cfg_fast = 1, cfg_trickprobe = 0, cfg_probe_ms = 2400, cfg_jump_tick = 1000, cfg_osd_argb = 1;       /* v46 */
static int cfg_fast_ref(void);
static int cfg_quiet_ref(void) { return cfg_quiet; }
static int cfg_m4cache_ref(void) { return cfg_m4cache; }
static int cfg_fast_ref(void) { return cfg_fast; }
static int cfg_play_secs = 0;                                                       /* v41: 0 = whole file */
static int cfg_v40 = 1, cfg_post = 0, cfg_framedump = 0;                            /* v40 (v45: frame dump off by default) */
static int cfg_gfx_seq = 1, cfg_dc_extra = 0, cfg_fbtest = 1;                       /* v38 */
static int cfg_dc_model = 0, cfg_dc_final = 99, cfg_dc_noinit = 0, cfg_gfx_quick = 1, cfg_pm27 = 1;   /* v37 */
static int cfg_gfx_screen = 1;                                                     /* v36 */
static int cfg_gfx = 0;                                                            /* v34 */
static int cfg_lcdtest = 0, cfg_dump_sdram = 1, cfg_dump_sram = 0;                /* v33 */
static int cfg_dump = 0, cfg_dump_len = 0x140000, cfg_dump_at1 = 20, cfg_dump_at2 = 35, cfg_vde = 0;   /* v32: VC RAM snapshots, vmcs_display_* probe */       /* v31: display_control power sequence; mid-play probe time in s (0 = off) */
static void bcm_load_cfg(void)
{
    static char buf[2048]; char *p, *nl, *eq;           /* v45: 2 KB (512 cut longer cfg files) */
    int fd = open("/.rockbox/bcm_test.cfg", O_RDONLY), n, v;
    if (fd < 0) { BLOG("no /.rockbox/bcm_test.cfg -- defaults"); return; }
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (p = buf; p && *p; p = nl ? nl + 1 : NULL)
    {
        nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (nl && nl > p && nl[-1] == '\r') nl[-1] = 0;
        eq = strchr(p, '=');
        if (!eq || p[0] == '#') continue;
        *eq = 0; v = atoi(eq + 1);
        if      (!strcmp(p, "si_ts"))    BLOG("cfg: si_ts is obsolete (the word is the bit rate; use si_w8)");
        else if (!strcmp(p, "si_w8"))    cfg_si_w8 = v;
        else if (!strcmp(p, "si_dur"))   cfg_si_dur = v;
        else if (!strcmp(p, "si_hw"))    cfg_si_hw = v;
        else if (!strcmp(p, "si_dim"))   cfg_si_dim = v;
        else if (!strcmp(p, "si_w1"))    cfg_si_w1 = v;
        else if (!strcmp(p, "si_last"))  cfg_si_last = v;
        else if (!strcmp(p, "si_count")) cfg_si_count = v;
        else if (!strcmp(p, "f2"))       cfg_f2 = v;
        else if (!strcmp(p, "map"))      cfg_map = v;
        else if (!strcmp(p, "reply_ms")) cfg_reply_ms = v;
        else if (!strcmp(p, "skip_disp")) cfg_skip_disp = v;
        else if (!strcmp(p, "sel_wait")) cfg_sel_wait = v;
        else if (!strcmp(p, "pm"))       cfg_pm = v;
        else if (!strcmp(p, "step"))     cfg_step = v;
        else if (!strcmp(p, "cmds_end")) cfg_cmds_end = v;
        else if (!strcmp(p, "reboot_end")) cfg_reboot_end = v;
        else if (!strcmp(p, "si_extra")) cfg_si_extra = v;
        else if (!strcmp(p, "fix1f8"))   cfg_fix1f8 = v;
        else if (!strcmp(p, "nolcd"))    cfg_nolcd = v;
        else if (!strcmp(p, "audio"))    cfg_audio = v;
        else if (!strcmp(p, "frame_avcc")) cfg_frame_avcc = v;
        else if (!strcmp(p, "si_b20"))   cfg_si_b20 = v;
        else if (!strcmp(p, "noreply"))  cfg_noreply = v;
        else if (!strcmp(p, "minimal"))  cfg_minimal = v;
        else if (!strcmp(p, "variant"))  cfg_variant = (eq[1] == 'a') ? 99 : v;     /* "auto" = 99 */
        else if (!strcmp(p, "region_when")) cfg_region_when = v;
        else if (!strcmp(p, "region_mode")) cfg_region_mode = v;
        else if (!strcmp(p, "region_w")) cfg_region_w = v;
        else if (!strcmp(p, "region_h")) cfg_region_h = v;
        else if (!strcmp(p, "pm_play"))  cfg_pm_play = v;
        else if (!strcmp(p, "disp_all")) cfg_disp_all = v;
        else if (!strcmp(p, "setvol"))   cfg_setvol = v;
        else if (!strcmp(p, "vchr47"))   cfg_vchr47 = v;
        else if (!strcmp(p, "freeze"))   cfg_freeze = v;
        else if (!strcmp(p, "flags_legacy")) cfg_flags_legacy = v;
        else if (!strcmp(p, "dc_seq"))   cfg_dc_seq = v;
        else if (!strcmp(p, "probe_at")) cfg_probe_at = v;
        else if (!strcmp(p, "dump"))     cfg_dump = v;
        else if (!strcmp(p, "dump_len")) cfg_dump_len = v;
        else if (!strcmp(p, "dump_at1")) cfg_dump_at1 = v;
        else if (!strcmp(p, "dump_at2")) cfg_dump_at2 = v;
        else if (!strcmp(p, "vde"))      cfg_vde = v;
        else if (!strcmp(p, "lcdtest"))  cfg_lcdtest = v;
        else if (!strcmp(p, "gfx"))      cfg_gfx = v;
        else if (!strcmp(p, "gfx_screen")) cfg_gfx_screen = v;
        else if (!strcmp(p, "dc_model")) cfg_dc_model = v;
        else if (!strcmp(p, "dc_final")) cfg_dc_final = v;
        else if (!strcmp(p, "dc_noinit")) cfg_dc_noinit = v;
        else if (!strcmp(p, "gfx_quick")) cfg_gfx_quick = v;
        else if (!strcmp(p, "pm27"))     cfg_pm27 = v;
        else if (!strcmp(p, "gfx_seq"))  cfg_gfx_seq = v;
        else if (!strcmp(p, "v40"))      cfg_v40 = v;
        else if (!strcmp(p, "play_secs")) cfg_play_secs = v;
        else if (!strcmp(p, "osd"))      cfg_osd = v;
        else if (!strcmp(p, "osd_layer")) cfg_osd_layer = v;
        else if (!strcmp(p, "subs"))     cfg_subs = v;
        else if (!strcmp(p, "subs_layer")) cfg_subs_layer = v;
        else if (!strcmp(p, "subs_sync")) cfg_subs_sync = v < 200 ? 200 : v;
        else if (!strcmp(p, "subs_delay")) cfg_subs_delay = v;
        else if (!strcmp(p, "scrub_mode")) cfg_scrub_mode = v;
        else if (!strcmp(p, "scrub_tick")) cfg_scrub_tick = v < 100 ? 100 : v;
        else if (!strcmp(p, "step_ms"))  cfg_step_ms = v;
        else if (!strcmp(p, "tap_secs")) cfg_tap_secs = v;
        else if (!strcmp(p, "ff_rate"))  cfg_ff_rate = v;
        else if (!strcmp(p, "log_sync")) cfg_log_sync = v;
        else if (!strcmp(p, "seekmon"))  cfg_seekmon = v;
        else if (!strcmp(p, "quiet"))    cfg_quiet = v;
        else if (!strcmp(p, "m4cache"))  cfg_m4cache = v;
        else if (!strcmp(p, "fast"))     cfg_fast = v;
        else if (!strcmp(p, "ff_accel")) cfg_ff_accel = v;
        else if (!strcmp(p, "sound"))    cfg_sound = v;
        else if (!strcmp(p, "av_tol"))   cfg_av_tol = v < 40 ? 40 : v;
        else if (!strcmp(p, "av_lead"))  cfg_av_lead = v;
        else if (!strcmp(p, "av_ready")) cfg_av_ready = v;
        else if (!strcmp(p, "asc_n"))    cfg_asc_n = v;
        else if (!strcmp(p, "av_offset")) cfg_av_offset = v;
        else if (!strcmp(p, "av_stall")) cfg_av_stall = v;
        else if (!strcmp(p, "av_hz"))    cfg_av_hz = v;
        else if (!strcmp(p, "av_mix"))   cfg_av_mix = v;
        else if (!strcmp(p, "vcvol"))    cfg_vcvol = v;
        else if (!strcmp(p, "vol_direct")) cfg_vol_direct = v;
        else if (!strcmp(p, "tv"))       cfg_tv = v;
        else if (!strcmp(p, "tv_enc"))   cfg_tv_enc = v;
        else if (!strcmp(p, "tv_extra")) cfg_tv_extra = v;
        else if (!strcmp(p, "tv_osd"))   cfg_tv_osd = v;
        else if (!strcmp(p, "boost"))    cfg_boost = v;
        else if (!strcmp(p, "ra_kb"))    cfg_ra_kb = v;
        else if (!strcmp(p, "moov_kb"))  cfg_moov_kb = v;
        else if (!strcmp(p, "ra_sleep")) cfg_ra_sleep = v;
        else if (!strcmp(p, "ra_mode"))  cfg_ra_mode = v;
        else if (!strcmp(p, "vol_start")) cfg_vol_start = v;
        else if (!strcmp(p, "vol_step")) cfg_vol_step = v < 1 ? 1 : v;
        else if (!strcmp(p, "trickprobe")) cfg_trickprobe = v;
        else if (!strcmp(p, "prep"))   cfg_prep = v;
        else if (!strcmp(p, "ff_resume")) cfg_ff_resume = v;
        else if (!strcmp(p, "probe_ms")) cfg_probe_ms = v < 500 ? 500 : v;
        else if (!strcmp(p, "jump_tick")) cfg_jump_tick = v < 300 ? 300 : v;
        else if (!strcmp(p, "osd_argb")) cfg_osd_argb = v;
        else if (!strcmp(p, "post"))     cfg_post = v;
        else if (!strcmp(p, "framedump")) cfg_framedump = v;
        else if (!strcmp(p, "dc_extra")) cfg_dc_extra = v;
        else if (!strcmp(p, "fbtest"))   cfg_fbtest = v;
        else if (!strcmp(p, "dump_sdram")) cfg_dump_sdram = v;
        else if (!strcmp(p, "dump_sram")) cfg_dump_sram = v;
        else BLOG("cfg: unknown key '%s'", p);
    }
    BLOG("cfg2: region_when=%d region_mode=%d region=%dx%d pm_play=%d disp_all=%d", cfg_region_when, cfg_region_mode, cfg_region_w, cfg_region_h, cfg_pm_play, cfg_disp_all);
    BLOG("cfg: si_w8=%d si_dur=%d si_hw=%d si_dim=%d si_w1=%d si_last=%d(-1=auto) si_count=%d f2=%d map=%d reply_ms=%d skip_disp=%d sel_wait=%d pm=%d step=%d cmds_end=%d",
         cfg_si_w8, cfg_si_dur, cfg_si_hw, cfg_si_dim, cfg_si_w1, cfg_si_last, cfg_si_count, cfg_f2, cfg_map, cfg_reply_ms, cfg_skip_disp, cfg_sel_wait, cfg_pm, cfg_step, cfg_cmds_end);
}

/* v22: the v21 log shows the VC core stops right after it consumes our 0x60 reply (selectplay is answered within 1 ms, then
   nothing: no further gencmd is read, its ack bits for ch0/ch2/ch3 never toggle, none of 6 RAM tick counters moves).
   Which part of the payload kills it is unknown, so each run can try one preset. */
#define BCM_NVARIANTS 1
static const char * const bcm_variant_name[BCM_NVARIANTS] = {
    "0: v52 PLAYER: Annex B decode, power=2 twice, video on top, stock overlay.  MENU = stop, PLAY = pause, SELECT = overlay, hold >>| / |<< = FF / REW, tap = +/-10 s"
};
static int bcm_variant_next = -1;               /* v25: counter value to store once the variant has really been exercised */
static void bcm_variant_commit(void)
{
    char c;
    int fd;
    if (bcm_variant_next < 0) return;
    c = (char)('0' + bcm_variant_next);
    bcm_variant_next = -1;
    fd = open("/.rockbox/bcm_variant.cnt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { write(fd, &c, 1); close(fd); }
}
static void bcm_apply_variant(void)
{
    int v = cfg_variant;
    if (v == 99)                                   /* auto: counter file */
    {
        char c = '0'; int fd = open("/.rockbox/bcm_variant.cnt", O_RDONLY);
        if (fd >= 0) { if (read(fd, &c, 1) != 1) c = '0'; close(fd); }
        v = (c >= '0' && c <= '9') ? c - '0' : 0;
        v %= BCM_NVARIANTS;
        bcm_variant_next = (v + 1) % BCM_NVARIANTS;     /* committed in v_start: a run that dies before the 0x60 does not use up a variant */
    }
    if (v < 0) return;
    if (v >= BCM_NVARIANTS) v = 0;
    cfg_si_extra = 1; cfg_si_b20 = 1; cfg_noreply = 0; cfg_minimal = 0; cfg_vchr47 = 0; cfg_flags_legacy = 0;
    cfg_region_when = 1; cfg_region_mode = 0; cfg_dc_seq = 0; cfg_vde = 0; cfg_dump = (v == 0); cfg_nolcd = 1;
    cfg_freeze = 2; cfg_lcdtest = 0; cfg_gfx = 1; cfg_dump = 0; cfg_dc_model = 1; cfg_dc_final = 2; cfg_dc_noinit = 1; cfg_dc_extra = 0;
    cfg_frame_avcc = 0; cfg_si_extra = 0; cfg_si_last = 0;       /* v40: the v39 V2 stream config -- the first one the decoder accepted */
    cfg_v40 = 1; cfg_post = 1;                     /* v44: status-screen presents stop at mp_play (bus time goes to the frames) */
    BLOG("VARIANT %s", bcm_variant_name[v]);
}

#define SI_WORDS 0x148
static uint32_t si_words[SI_WORDS];
static int bcm_send_stream_info(void)
{
    uint32_t i = 8;
    memset(si_words, 0, sizeof si_words);
    si_words[0] = (uint32_t)cfg_si_count;              /* stream count (#handler.num_streams) */
    si_words[1] = cfg_si_dur >= 0 ? (uint32_t)cfg_si_dur : m4_dur_ms;   /* GUESS: duration, ms (#handler.duration; stock [this+0xc]) */
    (void)cfg_si_w1;
    if (cfg_minimal)                                   /* variant 5: zero streams, everything else zero */
    {
        memset(si_words, 0, sizeof si_words);
        BLOG("stream info (0x60): MINIMAL (zero streams, all-zero payload)");
        return bcm_pds_send_stream_info(si_words, sizeof si_words);
    }
    si_words[2] = 1; si_words[4] = 1; si_words[5] = 1; /* constants written by rt 0x288f40 (w3 = w6 = 0) */
    si_words[i++] = 1;                                 /* type: 1 = video */
    si_words[i++] = 0x48323634;                        /* 'H264' */
    si_words[i++] = cfg_si_w8 >= 0 ? (uint32_t)cfg_si_w8 : m4_br;   /* GUESS: bit rate, bit/s (#video.bit_rate) */
    si_words[i++] = 0;
    {   const uint8_t *ex = NULL; uint32_t exn = 0, k;                  /* N, then N words (one byte each) of extradata */
        if (cfg_si_extra == 1 && m4.avcc_len) { ex = m4.avcc; exn = m4.avcc_len; }
        else if (cfg_si_extra == 2)           { ex = m4.ps;   exn = m4.ps_len; }
        si_words[i++] = exn;
        for (k = 0; k < exn && i < SI_WORDS - 70; k++) si_words[i++] = ex[k];
    }
    si_words[i++] = cfg_si_dim ? (cfg_si_hw ? m4.height : m4.width) : 0;    /* GUESS (the handler lists height before width) */
    si_words[i++] = cfg_si_dim ? (cfg_si_hw ? m4.width : m4.height) : 0;
    si_words[i++] = (uint32_t)cfg_si_b20;              /* byte [s+0x20] (the stock code always writes 1) */
    si_words[i++] = 0;                                 /* byte [s+0x21]: 2 only when AES-encrypted */
    si_words[i++] = 0;                                 /* key bits (0 = none) */
    i += 32 + 16;                                      /* key + IV, zero */
    si_words[i++] = cfg_si_last >= 0 ? (uint32_t)cfg_si_last : (uint32_t)(m4.nal_len - 1);   /* stock: avcC lengthSizeMinusOne (3) */
    if (v_sound == 2 && ma.ok)
    {   /* v53: the stock route -- the AAC track as a second block (stream id 1), laid out like the stock RAM snapshot:
           type 0, 'AAC\0', bit rate, 0, N, N AudioSpecificConfig bytes (one per word), channels, sample rate,
           b20 = 1, b21 = 0, key bits 0, 48 words key + IV, last word 0 */
        uint32_t k, n = ma.asc_len;
        if (cfg_asc_n > 0 && n > (uint32_t)cfg_asc_n) n = (uint32_t)cfg_asc_n;
        si_words[0] = 2;
        si_words[i++] = 0;
        si_words[i++] = 0x00414143;                    /* 'AAC\0' */
        si_words[i++] = ma.bitrate ? ma.bitrate : 128000;
        si_words[i++] = 0;
        si_words[i++] = n;
        for (k = 0; k < n; k++) si_words[i++] = ma.asc[k];
        si_words[i++] = ma.channels;
        si_words[i++] = ma.rate;
        si_words[i++] = 1;
        si_words[i++] = 0;
        si_words[i++] = 0;
        i += 32 + 16;
        si_words[i++] = 0;
        BLOG("stream info (0x60): + AAC block (stream 1): %u Hz, %u ch, %u bit/s, ASC %u of %u bytes %02x %02x %02x %02x %02x",
             (unsigned)ma.rate, (unsigned)ma.channels, (unsigned)ma.bitrate, (unsigned)n, (unsigned)ma.asc_len,
             ma.asc[0], ma.asc[1], ma.asc[2], ma.asc[3], ma.asc[4]);
    }
    BLOG("stream info (0x60): extradata=%u bytes (si_extra=%d) b20=%d; duration_ms=%u bit_rate=%u; %u words used, w8..: %08x %08x %08x %08x %08x %08x %08x",
         (unsigned)m4.avcc_len * (cfg_si_extra == 1) + (unsigned)m4.ps_len * (cfg_si_extra == 2), cfg_si_extra, cfg_si_b20,
         (unsigned)si_words[1], (unsigned)si_words[10], (unsigned)i, (unsigned)si_words[8], (unsigned)si_words[9], (unsigned)si_words[10], (unsigned)si_words[11],
         (unsigned)si_words[12], (unsigned)si_words[13], (unsigned)si_words[14]);
    return bcm_pds_send_stream_info(si_words, sizeof si_words);
}

/* v13: PDS callbacks that really feed the test video. Stream/buffer mapping, the meaning of f2/f4/meta[]
   and whether the VC wants Annex B are all UNVERIFIED (see bcm_host.h): everything the VC asks is
   logged for the first frames so the next run can correct them. Video only -- audio (AAC) is NOT fed;
   any request for a stream other than the video one is answered "nothing" (send_empty). */
static int v_frames, v_bytes, v_getframes, v_empty, v_eos, v_seeks, v_skips;
static int a_frames, a_bytes, a_eos, a_req;   /* v53: AAC fed to the VC (stock route) */
static uint32_t a_last_ts;
static char v_path[MAX_PATH];        /* v51: the video being played (for the sound) */
/* v46 trick feed: while tf_dir != 0 each request gets the next KEY frame in that direction (tf_jump ms on), real timestamps */
static int tf_dir, tf_fed;
static uint32_t tf_cur, tf_jump = 2000;
static uint32_t tf_next_key(void);
static bool v_stop, v_live;
static long v_eos_tick;
static void v_start(int id, uint32_t w1)
{
    BLOG("  PDS start: object=%d w1=%08x", id, (unsigned)w1);
    m4_rewind();
    if (ma.ok) ma_rewind();
    a_frames = a_bytes = 0;
    if (cfg_reply_ms > 0) sleep(cfg_reply_ms * HZ / 1000 ? cfg_reply_ms * HZ / 1000 : 1);
    /* v15: the 0x60 stream description is the REPLY to this 0x41 (seq echoed) -- the VC waits for it. */
    bcm_variant_commit();
    if (cfg_noreply) { BLOG("  VARIANT: NOT sending the 0x60 reply"); return; }
    BLOG("  0x60 stream-info reply sent = %d", bcm_send_stream_info());
}
static void v_get_frame(int id, uint32_t w1, uint32_t w2)
{
    uint32_t ts = 0; bool key = false; int n, tries = 0, want = (int)w2, r;
    struct bcm_pds_frame f;
    v_getframes++;
    if (v_getframes <= 12)
        BLOG("  PDS get_frame #%d: cur_stream=%d w1=%08x w2=%08x", v_getframes, id, (unsigned)w1, (unsigned)w2);
    if (v_sound == 2 && ma.ok && !tf_dir && (want == 1 || want == -1))
    {   /* v53: stock rt 0x20b39c -- "any stream" gets the one that is behind (lower next timestamp) */
        uint32_t ta = ma_next_ts(), tv = m4_next_ts();
        if (want == 1 || (ta != 0xffffffffu && ta <= tv))
        {
            a_req++;
            do n = ma_next(&ts); while (n < 0 && ++tries < 8);
            if (n <= 0)
            {
                if (!a_eos) BLOG("  audio: end of the AAC track after %d frames", a_frames);
                a_eos++; bcm_pds_send_empty();
                return;
            }
            memset(&f, 0, sizeof f);
            f.stream_id = 1; f.f2 = 1; f.f4 = ts;      /* stock: audio f2 = 1, f4 = time */
            f.length = (uint32_t)n; f.data = ma_buf;
            r = bcm_pds_send_frame(&f);
            a_frames++; a_bytes += n; a_last_ts = ts;
            if (a_frames <= 6 || a_frames % 2000 == 0 || r < 0)
                BLOG("  aac #%d len=%d ts=%ums send=%d (want=%d)", a_frames, n, (unsigned)ts, r, want);
            return;
        }
    }
    if (want != -1 && want != 0)                        /* not the video stream (assumed 0 or "any") */
    {
        v_empty++; bcm_pds_send_empty();
        if (v_empty <= 4) BLOG("  -> not video (want=%d): sent empty", want);
        return;
    }
    if (tf_dir)
    {
        uint32_t km = tf_next_key();
        tf_cur = km; m4_seek_sample(m4_key_at(km, NULL)); tf_fed++;
        if (tf_fed <= 3 || tf_fed % 20 == 0) BLOG("  trick feed #%d: key frame at %u ms (%s)", tf_fed, (unsigned)km, tf_dir > 0 ? "fwd" : "back");
    }
    do { n = m4_next(&ts, &key); if (n < 0) v_skips++; } while (n < 0 && ++tries < 8);
    if (n <= 0)
    {
        if (!v_eos) { BLOG("  end of stream after %d frames (%d bytes)", v_frames, v_bytes); v_eos_tick = current_tick; }
        v_eos++; bcm_pds_send_empty();
        return;
    }
    memset(&f, 0, sizeof f);
    (void)id;                                           /* v14: `id` is the object id string from the 0x41 message, NOT a stream id */
    f.stream_id = 0;                                    /* video-only file: video is stream 0 (ids count accepted streams in file order) */
    f.f2 = cfg_f2 == 2 ? (key ? 1 : 0) : (uint16_t)cfg_f2;                                 /* GUESS: stock passes a per-sample word here for video (1 for audio) */
    f.f4 = ts;                                          /* timestamp, ms -- matches the timescale (1000) sent in the 0x60 message */
    f.length = (uint32_t)n; f.data = m4_out;
    r = bcm_pds_send_frame(&f);
    v_frames++; v_bytes += n;
    if (v_frames <= 12 || v_frames % (cfg_quiet ? 900 : 30) == 0 || r < 0)
        BLOG("  frame #%d sample=%u len=%d key=%d ts=%ums send=%d", v_frames, (unsigned)m4.idx, n, key, (unsigned)ts, r);
}
static volatile uint32_t v_seek_kms;      /* v78: where the VC's last PDS seek landed (the key frame), and how many */
static volatile int v_seek_n;
static void v_seek(int id, uint32_t pos)
{
    uint32_t k, kms;
    v_seeks++;
    if (pos == 0) { m4_rewind(); m4.need_ps = true; if (ma.ok) ma_rewind(); a_eos = 0; v_seek_kms = 0; v_seek_n++; BLOG("  PDS seek: stream=%d pos=0 -> rewind", id); return; }
    k = m4_key_at(pos, &kms);                          /* v42: pos = milliseconds (it is the mp_seek argument) */
    v_seek_kms = kms; v_seek_n++;
    m4_seek_sample(k);
    if (ma.ok) { ma_seek_ms(kms); a_eos = 0; }         /* v53: the sound restarts at the key frame too */
    BLOG("  PDS seek: stream=%d pos=%u ms -> key frame sample %u at %u ms (chunk %u +%u)", id, (unsigned)pos, (unsigned)k, (unsigned)kms,
         (unsigned)m4.chunk, (unsigned)m4.off_in_chunk);
}
static void v_stopcb(void) { BLOG("  PDS stop (frames sent so far: %d)", v_frames); v_stop = true; }
static const struct bcm_pds_ops pds_video_ops = { v_start, v_get_frame, v_seek, v_stopcb };

/* Service loop while the VC plays: ~100 Hz, status line every 2 s, mp_get_stats every 5 s. */
/* v32: copy VC memory to a file in 4 KB pieces, servicing the host interface between pieces so that frames keep flowing. */
static void bcm_vc_dump(const char *tag, uint32_t start, uint32_t len)
{
    static uint32_t dbuf[1024];
    char path[48]; int fd; uint32_t a; long t0 = current_tick; unsigned n = 0;
    if ((!cfg_dump && strcmp(tag, "frame")) || bcm_io_dead()) return;   /* v41: the decoded-frame dump is always allowed */
    snprintf(path, sizeof path, "/.rockbox/vcdump_%s.bin", tag);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { BLOG("dump: cannot create %s", path); return; }
    for (a = start; a < start + len; a += sizeof dbuf, n++)
    {
        bcm_io_lock(); bcm_io_read(a, dbuf, sizeof dbuf); bcm_io_unlock();
        if (bcm_io_dead()) { BLOG("dump %s: bus died at VC 0x%08x", tag, (unsigned)a); break; }
        if (write(fd, dbuf, sizeof dbuf) != (ssize_t)sizeof dbuf) { BLOG("dump %s: write failed", tag); break; }
        if ((n & 3) == 3) bcm_host_service();
    }
    close(fd);
    BLOG("dump %s: VC 0x%08x..0x%08x -> %s (%u ms)", tag, (unsigned)start, (unsigned)a, path, (unsigned)((current_tick - t0) * 1000 / HZ));
}

/* v32: where is the rest of VC memory?  Read single words at candidate bases (aliases first; an unmapped read may time out and
   kill the bus, so this runs at the very end of the run).  VC[0x200] is compared with the aliases. */
static void bcm_map_probe(void)
{
    static const uint32_t cand[] = { 0x00000200, 0xFE000200, 0xC0000200, 0x80000200, 0x40000200, 0x013FFFFC, 0x00200000, 0x003FFFFC, 0x00400000, 0x007FFFFC, 0x00800000 };
    unsigned i;
    if (!cfg_dump || 1) return;   /* v33: map known (SDRAM at 0xC0000000, mirrored every 2 MB below) */
    BLOG("---- VC address map probe");
    for (i = 0; i < sizeof cand / sizeof cand[0]; i++)
    {
        uint32_t w = 0;
        BLOG("  reading VC 0x%08x ...", (unsigned)cand[i]);
        bcm_io_lock(); bcm_io_read(cand[i], &w, 4); bcm_io_unlock();
        if (bcm_io_dead()) { BLOG("  bus died on that read -- stop"); break; }
        BLOG("  VC[0x%08x] = %08x", (unsigned)cand[i], (unsigned)w);
    }
    BLOG("---- end of map probe");
}

/* v32: display_control power/model is all the stock ARM code ever sends, but vmcs.bin also registers vmcs_display_clear and
   vmcs_display_enable (never sent by the stock ARM image).  Variants 1/2 try them. */
static void bcm_vde(void)
{
    char resp[200]; int r;
    if (cfg_vde == 2)
    {
        bcm_cmd_t0 = current_tick;
        r = bcm_gencmd_watched("vmcs_display_clear", resp, sizeof resp, 3);
        bcm_log_text("vmcs_display_clear", r, resp);
    }
    if (cfg_vde)
    {
        bcm_cmd_t0 = current_tick;
        r = bcm_gencmd_watched("vmcs_display_enable 1", resp, sizeof resp, 3);
        bcm_log_text("vmcs_display_enable 1", r, resp);
    }
}

/* v33: the legacy LCD path.  Rockbox's lcd driver writes RGB565 rows (pitch 640 B) to VC 0xE0000 and then writes BCM_CMD(0) =
   0xFFFF0000 to VC[0x1F8]; the VC is supposed to copy the frame to the panel and change the word.  In every run so far
   VC[0x1F8] stays 0xFFFF0000, i.e. the VC ignores the kick.  This draws colour bars and kicks once, then polls the word. */
static void bcm_lcd_test(void)
{
    static uint16_t row[320];
    uint32_t cmd = 0xFFFF0000u, w = 0, one = 1;
    int x, y, i;
    if (!cfg_lcdtest || bcm_io_dead()) return;
    BLOG("---- LCD test: drawing colour bars into VC 0xE0000 (RGB565, 320x240)");
    for (y = 0; y < 240; y++)
    {
        for (x = 0; x < 320; x++)
        {
            int bar = x / 40;
            static const uint16_t col[8] = { 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF, 0xFFFF, 0x0000 };
            row[x] = (y < 24) ? (uint16_t)(((x * 31 / 319) << 11) | ((x * 63 / 319) << 5) | (x * 31 / 319)) : col[bar];
        }
        bcm_io_lock(); bcm_io_write(0xE0000 + 640 * (unsigned)y, row, sizeof row); bcm_io_unlock();
        if ((y & 31) == 31) bcm_host_service();
    }
    bcm_io_lock(); bcm_io_read(0x1F8, &w, 4); bcm_io_unlock();
    BLOG("  VC[1F8] before kick = %08x", (unsigned)w);
    bcm_io_lock(); bcm_io_write(0x1F8, &cmd, 4); bcm_io_doorbell(); bcm_io_unlock();
    for (i = 0; i < 20; i++)
    {
        long t = current_tick + HZ / 10;
        while (TIME_BEFORE(current_tick, t)) { bcm_host_service(); sleep(HZ / 100); }
        bcm_io_lock(); bcm_io_read(0x1F8, &w, 4); bcm_io_unlock();
        if (i < 3 || i == 19 || w != cmd) BLOG("  +%d ms: VC[1F8] = %08x", (i + 1) * 100, (unsigned)w);
        if (w != cmd) break;
    }
    BLOG("  >>> LOOK AT THE SCREEN NOW: colour bars (red green blue yellow magenta cyan white black, gradient on top)? (5 s)");
    {   long t = current_tick + 5 * HZ;
        while (TIME_BEFORE(current_tick, t)) { bcm_host_service(); sleep(HZ / 100); } }
    bcm_io_lock(); bcm_io_write(0x1F8, &one, 4); bcm_io_unlock();
    BLOG("---- end of LCD test");
}

/* v34: the stock firmware does not use the legacy LCD mailbox with this VC image.  Its display class (rt 0x164450..)
   allocates an image on the VC (graphics channel op 8 -> handle + VC address), copies the pixel rows to that address
   over the bus, and shows the image as a "window" (ops 1 = update start, 3 = add window, 2 = update end; 4 = remove
   window, 9 = free image).  The argument meanings of op 3 are only partly known, so this tries three guesses. */
static int bcm_gfx_call(const char *tag, uint32_t op, const void *in, uint32_t inlen, uint32_t out[4])
{
    uint32_t o[4] = { 0, 0, 0, 0 }, rop = 0xdeadbeef;
    int r = bcm_gfx_rpc(op, in, inlen, o, &rop, 300);
    BLOG("  gfx op %u (%s): r=%d reply-op=%08x payload=%08x %08x %08x %08x", (unsigned)op, tag, r, (unsigned)rop,
         (unsigned)o[0], (unsigned)o[1], (unsigned)o[2], (unsigned)o[3]);
    if (out) memcpy(out, o, sizeof o);
    return r;
}
static void bcm_hold_ms(int ms)
{
    long t;
    if (cfg_fast_ref() && ms > 150) ms = 150;        /* v46 fast start: no "LOOK" pauses */
    if (cfg_fast_ref() && bcm_player_mode && ms > 30) ms = 30;   /* v56: player: just let the panel settle */
    t = current_tick + (HZ * ms) / 1000;
    while (TIME_BEFORE(current_tick, t)) { bcm_host_service(); sleep(HZ / 100); }
}
/* v36: a stock-like double-buffered UI layer (display class rt 0x164f44 present / rt 0x1b02d8 boot-time UI layer):
   two VC images, each frame: op 1, op 4 (remove the front window), bulk-write the back image, op 3 (window for the back image,
   A=0 B=-50 x=y=0), swap.  The picture is the Rockbox frame buffer, into which we draw a live status screen. */
static uint32_t gfx_img[2], gfx_addr[2], gfx_win[2], gfx_black[2];
static int gfx_front = -1, gfx_presents;
static long gfx_next;
static int bcm_gfx_alloc(uint32_t *img, uint32_t *addr)
{
    uint32_t pl[8], o[4];
    memset(pl, 0, sizeof pl);
    ((uint8_t *)pl)[4] = 1; pl[2] = LCD_WIDTH; pl[3] = LCD_HEIGHT; pl[4] = LCD_WIDTH * 2;
    ((uint8_t *)pl)[0x1c] = 1;
    if (bcm_gfx_call("create image", 8, pl, 0x20, o) != 0 || !o[0] || o[1] < 0x1000) return -1;
    *img = o[0]; *addr = o[1];
    return 0;
}
static void bcm_gfx_draw_status(void)
{
    static int phase;
    long e = current_tick - bcm_t_start;
    if (bcm_player_mode)
        return;     /* v48.1: the player never touches Rockbox's frame buffer or LCD settings (v48 cleared them and broke the theme) */
    lcd_set_viewport(NULL);                 /* full screen, absolute coordinates */
    lcd_set_backdrop(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_background(LCD_RGBPACK(0, 0, 48));
    lcd_set_foreground(LCD_RGBPACK(255, 255, 255));
    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    lcd_putsxyf(4, 4, "bcm host test v79 - VC graphics layer");
    lcd_putsxyf(4, 16, "this screen = Rockbox FB -> VC image %d", gfx_front < 0 ? 0 : 1 - gfx_front);
    lcd_putsxyf(4, 28, "t=%ld.%lds  presents=%d", e / HZ, (e % HZ) / 10, gfx_presents);
    lcd_putsxyf(4, 40, "video: sent=%d frames (%d KB)", v_frames, v_bytes / 1024);
    lcd_putsxyf(4, 52, "get_frame=%d empty=%d seeks=%d", v_getframes, v_empty, v_seeks);
    lcd_set_foreground(LCD_RGBPACK(255, 255, 0));
    lcd_putsxyf(4, 70, "%.52s", bcm_last_line);
    lcd_set_foreground(LCD_RGBPACK(255, 0, 0));
    lcd_fillrect((phase * 8) % (LCD_WIDTH - 40), 200, 40, 20);      /* moves every present: proves updates arrive */
    phase++;
}
static void bcm_gfx_present_m(int mode)
{
    uint32_t o[4], pl[8], w4[4];
    int back, y;
    const fb_data *fb;
    if (gfx_front < 0 || bcm_io_dead()) return;
    back = 1 - gfx_front;
    static const fb_data black[16 * LCD_WIDTH];
    bcm_gfx_draw_status();
    fb = FBADDR(0, 0);
    gfx_presents++;
    {   int verbose = gfx_presents <= 3 || (gfx_presents % 20) == 0;
        if (verbose) BLOG("  gfx present #%d: back image %d (%08x @ VC %08x)", gfx_presents, back, (unsigned)gfx_img[back], (unsigned)gfx_addr[back]);
        bcm_gfx_rpc(1, NULL, 0, o, NULL, 300);
        if (verbose || mode >= 0x100) BLOG("    op1 status %08x %08x", (unsigned)o[0], (unsigned)o[1]);
        if (gfx_win[gfx_front])
        {
            memset(w4, 0, sizeof w4); w4[0] = gfx_win[gfx_front];
            bcm_gfx_rpc(4, w4, 0x10, o, NULL, 300);
            gfx_win[gfx_front] = 0;
        }
        if (mode & 2)
        {
            uint32_t t[4] = { gfx_img[back], 1, 0, 0 };
            bcm_gfx_rpc(0x10, t, 8, o, NULL, 300);
        }
        for (y = 0; y < LCD_HEIGHT && !(bcm_player_mode && gfx_black[back] == gfx_addr[back]); y += 16)
        {   /* v56: player mode: each image is black after its first write -- do not send 150 KB of zeros again */
            bcm_io_lock();
            bcm_io_write(gfx_addr[back] + (unsigned)y * LCD_WIDTH * 2, bcm_player_mode ? black : fb + y * LCD_WIDTH, 16 * LCD_WIDTH * 2);
            bcm_io_unlock();
            bcm_host_service();
        }
        if (bcm_player_mode) gfx_black[back] = gfx_addr[back];
        if (gfx_presents <= 2 && !bcm_player_mode)
        {
            uint16_t rb[8];
            bcm_io_lock(); bcm_io_read(gfx_addr[back] + 640u * 200u, rb, sizeof rb); bcm_io_unlock();
            BLOG("    readback VC %08x (row 200): %04x %04x %04x %04x  ARM: %04x %04x %04x %04x", (unsigned)(gfx_addr[back] + 640u * 200u),
                 rb[0], rb[1], rb[2], rb[3], fb[200 * LCD_WIDTH], fb[200 * LCD_WIDTH + 1], fb[200 * LCD_WIDTH + 2], fb[200 * LCD_WIDTH + 3]);
        }
        memset(pl, 0, sizeof pl);
        pl[0] = (mode & 4) ? 2 : 0; pl[1] = (mode & 8) ? 0 : 0xffffffceu; pl[3] = gfx_img[back];
        ((uint16_t *)pl)[12] = LCD_WIDTH; ((uint16_t *)pl)[13] = LCD_HEIGHT;
        if (bcm_gfx_rpc(3, pl, 0x20, o, NULL, 300) == 0) gfx_win[back] = o[0];
        if (mode & 1) bcm_gfx_rpc(2, NULL, 0, o, NULL, 300);
        if (verbose || mode >= 0x100) BLOG("    -> window %08x (mode %x)", (unsigned)gfx_win[back], (unsigned)mode);
    }
    gfx_front = back;
}
static void bcm_gfx_present(void) { bcm_gfx_present_m(cfg_gfx_seq); }

static void bcm_gfx_probe(void)
{
    static uint16_t row[320];
    uint32_t o[4], img = 0, vcaddr = 0, win = 0, pl[8];
    int x, y, g;
    if (!cfg_gfx || bcm_io_dead()) return;
    BLOG("---- GFX probe (graphics channel type 2)");
    bcm_gfx_call("update start?", 1, NULL, 0, o);
    bcm_gfx_call("update end?", 2, NULL, 0, o);
    memset(pl, 0, sizeof pl);
    ((uint8_t *)pl)[4] = 1;                    /* format 1 = RGB565 */
    pl[2] = 320; pl[3] = 240; pl[4] = 640;     /* width, height, pitch; pl[6] = VC address 0 -> allocate */
    ((uint8_t *)pl)[0x1c] = 1;
    if (bcm_gfx_call("create image 320x240 RGB565", 8, pl, 0x20, o) != 0) { BLOG("  no reply to op 8 -> stop"); return; }
    img = o[0]; vcaddr = o[1];
    BLOG("  image handle %08x, VC address %08x", (unsigned)img, (unsigned)vcaddr);
    if (!img || vcaddr < 0x1000) { BLOG("  unusable handle/address -> stop"); return; }
    for (y = 0; y < 240; y++)
    {
        static const uint16_t col[8] = { 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF, 0xFFFF, 0x0000 };
        for (x = 0; x < 320; x++)
            row[x] = bcm_player_mode ? 0 : (y < 24) ? (uint16_t)(((x * 31 / 319) << 11) | ((x * 63 / 319) << 5) | (x * 31 / 319)) : col[x / 40];
        bcm_io_lock(); bcm_io_write(vcaddr + 640u * (unsigned)y, row, sizeof row); bcm_io_unlock();
        if ((y & 31) == 31) bcm_host_service();
    }
    BLOG("  pixels written to VC %08x", (unsigned)vcaddr);
    bcm_gfx_call("touch image?", 0x10, (uint32_t[]){ img, 0 }, 8, o);
    /* v35: the layer-create method (rt 0x1649ac) shows op 3 = {A, B, 0, image, x:u16, y:u16, 0, 0, w:u16, h:u16}:
       x,y = position (updated through [layer+0x2c/0x30]), w,h = source size (layer+8/+0xc), A in {0,2}, B an int that is -50 for the
       video-behind layer.  The stock present path sends op 1, op 3 and NO op 2; op 2 only appears in the layer release (op 1, 4, 2). */
    static const struct { uint32_t a, b; int op2; } gt[5] = {
        { 0, 0u,          0 }, { 0, 0u,          1 }, { 2, 0xffffffceu, 1 }, { 0, 0xffffffceu, 1 },
        { 0, 0xffffffceu, 0 }    /* v36: exactly what the stock firmware does right after booting vmcs.bin (rt 0x1b02d8): A=0, B=-50, x=y=0, no op 2 */
    };
    for (g = cfg_gfx_quick ? 4 : 0; g < 5; g++)
    {
        memset(pl, 0, sizeof pl);
        pl[0] = gt[g].a; pl[1] = gt[g].b; pl[3] = img;
        ((uint16_t *)pl)[8] = 0; ((uint16_t *)pl)[9] = 0;            /* x, y */
        ((uint16_t *)pl)[12] = 320; ((uint16_t *)pl)[13] = 240;      /* w, h */
        BLOG("  -- window guess %d: A=%u B=%d op2-after=%d", g, (unsigned)gt[g].a, (int)gt[g].b, gt[g].op2);
        bcm_gfx_call("status", 1, NULL, 0, o);
        if (bcm_gfx_call("add window", 3, pl, 0x20, o) == 0) win = o[0]; else win = 0;
        if (gt[g].op2) bcm_gfx_call("op2", 2, NULL, 0, o);
        BLOG("  >>> LOOK AT THE SCREEN: guess %d, window handle %08x (2.5 s)", g, (unsigned)win);
        bcm_hold_ms(2500);
        if (win && g < 4)
        {
            uint32_t w4[4] = { win, 0, 0, 0 };
            bcm_gfx_call("status", 1, NULL, 0, o);
            bcm_gfx_call("remove window", 4, w4, 0x10, o);
            bcm_gfx_call("op2", 2, NULL, 0, o);
        }
    }
    BLOG("  last window (guess 4 = stock-exact) and the image are left in place for the playback that follows");
    if (cfg_gfx_screen)
    {
        gfx_img[0] = img; gfx_addr[0] = vcaddr; gfx_win[0] = win;
        if (bcm_gfx_alloc(&gfx_img[1], &gfx_addr[1]) == 0)
        {
            gfx_front = 0; gfx_next = current_tick + HZ;
            BLOG("  gfx screen: second image %08x @ VC %08x; a status screen is presented once per second from now on",
                 (unsigned)gfx_img[1], (unsigned)gfx_addr[1]);
            bcm_gfx_present();
        }
    }
    BLOG("---- end of GFX probe");
    return;
    bcm_gfx_call("free image", 9, (uint32_t[]){ img, 0, 0, 0 }, 0x10, o);
    BLOG("---- end of GFX probe");
}

/* v31: one-shot probe while frames flow.  The v30 logs show mp_region always answers debug="region 0 0 0 0 0", even after
   mp_play, and the display stage (last_displayed_frame, Disp ...) never moves.  Send a few differently-shaped mp_region
   commands (the replies tell us what the five numbers mean) plus mp_paint, then dump mp_get_stats again. */
static void bcm_mid_probe(void)
{
    static const char * const pc[] = {
        "mp_region display=0 dest=\"0 0 320 240\" mode=fill",
        "mp_region display=0 dest=\"10 20 100 50\" mode=fill",
        "mp_region display=0 src=\"0 0 320 240\" dest=\"0 0 320 240\" mode=fill",
        "mp_region display=0 dest=fullscreen mode=fill",
        "mp_region display=0 dest=fullscreen mode=letterbox pixel=\"1 1\"",
        "mp_region display=0 dest=\"0 0 320 240\"",
        "mp_paint",
        "mp_get_stats video",
    };
    static char resp[1400];
    unsigned i; int r;
    BLOG("---- mid-play display probe");
    for (i = 0; i < sizeof pc / sizeof pc[0]; i++)
    {
        bcm_cmd_t0 = current_tick;
        r = bcm_gencmd_watched(pc[i], resp, sizeof resp, 3);
        bcm_log_text(pc[i], r, resp);
    }
    BLOG("---- end of probe");
}

/* ---------------------------------------------------------------- v46: player UI ----
   STOCK OVERLAY.  The stock firmware's UI resources (block at osos 0x3325a0: BMap/Bits/LDTm/Menu/Str/TCmd/Type/View, see
   findings 3ap) contain the video-playback overlay: views 22056 (root) -> 22058 status icon box at (15,15) 33x33 and
   22057 bottom panel at y=192 h=43 -> 22059 progress control (280x20, centred, bitmaps 21922 fill / 21923 frame) with the
   time labels 22062..22065 (digits 21939..21948, '-' 21949, ':' 21950, 10x17, left- and right-aligned under the bar),
   plus 30x27 play / pause / FF / REW icons 21931..21934.  They are ARGB4444 with soft shadows and are drawn straight over
   the video: the stock layer code (rt 0x164450) creates VC image type 15 (32-bit ARGB, the ARM surface format 0x1888 =
   0xAARRGGBB, packer rt 0x256388) for such surfaces and type 1 for RGB565 ones.
   We do the same: three ARGB windows (status icon, battery %, bottom panel) on layer osd_layer above the video.  The bitmaps
   are NOT in this source: they are read at run time from /.rockbox/bcm_video_ui.bin, cut from the iPod's own firmware
   (tools/bcm/extract_video_ui.py).  Without that file a look-alike drawn in code is used.
   Battery: the stock overlay has no battery; as asked, the battery % is shown top right in the stock digit style.

   FF/REW (v46): the v45 log shows "mp_seek + mp_step" leaves mplayer in mode=step with buffering=2450 and Disp seq_num
   frozen -- nothing is shown.  v46 measures the candidates once per run (trick probe, ~30 s after the start, buttons off)
   and uses what really moved the picture:
     ffk   mp_ff/mp_rewind (mplayer trick mode, rate 0xa00) while WE feed only key frames, forwards or backwards
     ff    mp_ff/mp_rewind with the normal feed
     step  repeated "mp_step <ms>" while playing (forward only)
     jump  fallback: a seek to the next key frame every second while playing
   Release always ends with mp_play + mp_seek to the key frame that was on screen (Disp time). */
#define VUI_PATH "/.rockbox/bcm_video_ui.bin"
enum { VUI_FILL = 0, VUI_FRAME, VUI_PLAY, VUI_PAUSE, VUI_FF, VUI_REW, VUI_D0 = 10, VUI_MINUS = 20, VUI_COLON, VUI_LOCK,
       VUI_SPK_LO, VUI_SPK_HI, VUI_VOL_FRAME, VUI_VOL_FILL, VUI_N };   /* v51: 23..26 = stock volume view 22060 */
struct vui_bmp { uint16_t w, h; const uint16_t *px; };
static struct vui_bmp vui[VUI_N];
static uint8_t vui_buf[80 * 1024] __attribute__((aligned(4)));
static int vui_state = -1;                      /* -1 not tried, 0 missing, 1 loaded */
static void vui_load(void)
{
    int fd, n; uint32_t cnt, i;
    if (vui_state >= 0) return;
    vui_state = 0;
    fd = open(VUI_PATH, O_RDONLY);
    if (fd < 0) { BLOG("  overlay: no %s -- using the drawn look-alike (copy the file from the v46 zip for the stock graphics)", VUI_PATH); return; }
    n = read(fd, vui_buf, sizeof vui_buf);
    close(fd);
    if (n < 8 || memcmp(vui_buf, "VUI1", 4)) { BLOG("  overlay: %s is not a VUI1 file", VUI_PATH); return; }
    memcpy(&cnt, vui_buf + 4, 4);
    for (i = 0; i < cnt && 8 + 12 * (i + 1) <= (uint32_t)n; i++)
    {
        uint16_t key, w, h, fmt; uint32_t off;
        memcpy(&key, vui_buf + 8 + 12 * i, 2); memcpy(&w, vui_buf + 10 + 12 * i, 2); memcpy(&h, vui_buf + 12 + 12 * i, 2);
        memcpy(&fmt, vui_buf + 14 + 12 * i, 2); memcpy(&off, vui_buf + 16 + 12 * i, 4);
        if (key >= VUI_N || fmt != 0x1444 || (off & 1) || off + 2u * w * h > (uint32_t)n) continue;
        vui[key].w = w; vui[key].h = h; vui[key].px = (const uint16_t *)(vui_buf + off);
    }
    vui_state = vui[VUI_FILL].px && vui[VUI_FRAME].px && vui[VUI_D0 + 9].px && vui[VUI_COLON].px ? 1 : 0;
    BLOG("  overlay: %s -> %s (%u bitmaps)", VUI_PATH, vui_state ? "stock graphics" : "incomplete, using the look-alike", (unsigned)cnt);
}

/* ---- ARGB canvas (0xAARRGGBB, straight alpha) */
struct cv { uint32_t *p; int w, h; };
static void cv_clear(struct cv *c) { memset(c->p, 0, (size_t)c->w * c->h * 4); }
static void cv_px(struct cv *c, int x, int y, uint32_t s)
{
    uint32_t a = s >> 24, d, da, oa, r, g, b;
    if (!a || x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    d = c->p[y * c->w + x]; da = d >> 24;
    if (a == 255 || !da) { c->p[y * c->w + x] = s; return; }
    oa = a + da * (255 - a) / 255;
    r = (((s >> 16) & 255) * a + ((d >> 16) & 255) * da * (255 - a) / 255) / oa;
    g = (((s >> 8) & 255) * a + ((d >> 8) & 255) * da * (255 - a) / 255) / oa;
    b = ((s & 255) * a + (d & 255) * da * (255 - a) / 255) / oa;
    c->p[y * c->w + x] = (oa << 24) | (r << 16) | (g << 8) | b;
}
static void cv_fill(struct cv *c, int x, int y, int w, int h, uint32_t s)
{ int i, j; for (j = 0; j < h; j++) for (i = 0; i < w; i++) cv_px(c, x + i, y + j, s); }
static uint32_t cv_tint = 0xFFFFFF;              /* v47: multiplies the bitmap colours (white digits -> battery colour) */
static uint32_t cv_tinted(uint32_t s)
{
    if (cv_tint == 0xFFFFFF) return s;
    return (s & 0xFF000000u) | ((((s >> 16) & 255) * ((cv_tint >> 16) & 255) / 255) << 16)
         | ((((s >> 8) & 255) * ((cv_tint >> 8) & 255) / 255) << 8) | ((s & 255) * (cv_tint & 255) / 255);
}
static void cv_blit(struct cv *c, int x, int y, int key, int clipw)
{
    const struct vui_bmp *b = &vui[key];
    int i, j, w;
    if (!b->px) return;
    w = clipw >= 0 && clipw < b->w ? clipw : b->w;
    for (j = 0; j < b->h; j++)
        for (i = 0; i < w; i++)
        {
            uint16_t v = b->px[j * b->w + i];
            uint32_t a = (v >> 12) & 15;
            if (a) cv_px(c, x + i, y + j, cv_tinted((a * 17u << 24) | (((v >> 8) & 15) * 17u << 16) | (((v >> 4) & 15) * 17u << 8) | ((v & 15) * 17u)));
        }
}
/* look-alike shapes when the stock bitmaps are missing: white with a dark shadow, like the stock art */
static void cv_shadowed(struct cv *c, int x, int y, int w, int h, uint32_t col)
{ cv_fill(c, x + 1, y + 1, w, h, 0x90000000); cv_fill(c, x, y, w, h, cv_tinted(col)); }
static const uint8_t lk_font[13][7] = {           /* 5x7 digits, '-', ':', '%' for the look-alike / the % sign */
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},{31,16,30,1,1,17,14},
    {6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12},{0,0,0,31,0,0,0},{0,12,12,0,12,12,0},
    {24,25,2,4,8,19,3} };
static int lk_glyph(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch == '-' ? 10 : ch == ':' ? 11 : ch == '%' ? 12 : -1; }
static int cv_glyph_w(char ch)
{
    if (vui_state == 1)
    {
        if (ch >= '0' && ch <= '9') return vui[VUI_D0 + ch - '0'].w;
        if (ch == '-') return vui[VUI_MINUS].w;
        if (ch == ':') return vui[VUI_COLON].w;
    }
    return ch == ':' ? 6 : 12;
}
static int cv_text_w(const char *s) { int w = 0; while (*s) w += cv_glyph_w(*s++); return w; }
static void cv_text(struct cv *c, int x, int y, const char *s)
{
    for (; *s; x += cv_glyph_w(*s), s++)
    {
        int k = -1, g, r, q;
        if (vui_state == 1)
        {
            if (*s >= '0' && *s <= '9') k = VUI_D0 + *s - '0'; else if (*s == '-') k = VUI_MINUS; else if (*s == ':') k = VUI_COLON;
            if (k >= 0 && vui[k].px) { cv_blit(c, x, y, k, -1); continue; }
        }
        g = lk_glyph(*s); if (g < 0) continue;
        for (r = 0; r < 7; r++) for (q = 0; q < 5; q++)          /* 5x7 scaled 2x -> 10x14, shadowed */
            if (lk_font[g][r] & (16 >> q)) cv_shadowed(c, x + (*s == ':' ? -2 : 1) + 2 * q, y + 2 + 2 * r, 2, 2, 0xFFF4F4F4);
    }
}

/* ---- windows */
#define OSD_NS 4
struct osd_strip { int x, y, w, h; uint32_t img[2], addr[2], win[2]; int front; uint32_t *cvp; uint32_t *shadow; uint32_t tvwin; };
static uint32_t osd_cv0[48 * 48], osd_cv1[112 * 24], osd_cv2[320 * 52], osd_cv3[96 * 80];
/* v56: what each VC image holds now -- updates write only the pixels that changed, straight into the visible image */
static uint32_t osd_sh0[48 * 48], osd_sh1[112 * 24], osd_sh2[320 * 52], osd_sh3[96 * 80];
static struct osd_strip osd_s[OSD_NS] = {
    /* status icon (stock box 33x33 at 15,15).  v47.1: 48x48 -- with a 40-pixel-wide image the icon came out sheared on the iPod:
       VC images evidently want widths in multiples of 16 pixels (the 80 and 320 wide windows were fine) */
    { 8, 8, 48, 48, { 0, 0 }, { 0, 0 }, { 0, 0 }, -1, osd_cv0, osd_sh0, 0 },
    { 200, 12, 112, 24, { 0, 0 }, { 0, 0 }, { 0, 0 }, -1, osd_cv1, osd_sh1, 0 },     /* hold lock, charging bolt, battery %, top right (v50: 112 wide) */
    { 0, 188, 320, 52, { 0, 0 }, { 0, 0 }, { 0, 0 }, -1, osd_cv2, osd_sh2, 0 },      /* stock bottom panel (y 192, h 43) */
    { 112, 78, 96, 80, { 0, 0 }, { 0, 0 }, { 0, 0 }, -1, osd_cv3, osd_sh3, 0 },     /* v55: TV pictogram in the middle of the LCD while the video is on the TV */
};
static int osd_ok = -1, osd_type = 15;
static bool osd_visible, pl_paused;
static long osd_hide_at, osd_next;
static uint32_t pl_dur_ms, pl_el_ms, pl_target_ms;
static char pl_title[40];
static int pl_buffering = -1;
static uint16_t osd_row[320 * 2];
static int av_vol_pct;                 /* v51: volume 0..100 % (audio section below) */
static long av_vol_until, av_vol_drawn;
static bool av_vol_dirty;
/* scrubber */
enum { SM_AUTO = 0, SM_SEEKSTEP = 1, SM_JUMP = 2, SM_FF = 3, SM_FFK = 4, SM_STEP = 5 };
static const char * const sm_name[6] = { "auto", "seek+step", "jump", "ff", "ff+keys", "step" };
static int sc_btn, sc_dir, sc_ticks, sc_method, sc_rate;
static bool sc_remote;                   /* v63: FF/REW started by a dock / remote (StartFF ... EndFFRew) */
static int sm_ff = SM_FFK, sm_rew = SM_FFK;      /* v47: the v46 probe: ff+keys 454 pictures / +21.5 s, rew+keys 452 / -22.2 s in 2.4 s */
static long sc_press, sc_t0, sc_next;
static bool sc_active, sc_was_playing;
static uint32_t sc_pos, sc_jump;
/* post-seek monitor */
static long mon_t0, mon_until, mon_next;
static int mon_req0;

static uint32_t pl_num(const char *r, const char *key)
{
    const char *q = strstr(r, key); uint32_t v = 0; bool neg = false;
    if (!q) return 0xffffffffu;
    q += strlen(key);
    if (*q == '-') { neg = true; q++; }
    while (*q >= '0' && *q <= '9') v = v * 10 + (uint32_t)(*q++ - '0');
    return neg ? (uint32_t)-(int32_t)v : v;
}
static long vr_el_tick;                  /* v63: when pl_el_ms was read (dock status reports estimate from it) */
static int pl_cmd(const char *c, bool log_ok)
{
    static char r[400]; int rr;
    bcm_cmd_t0 = current_tick;
    rr = bcm_gencmd_watched(c, r, sizeof r, 3);
    if (log_ok || rr != 0 || strstr(r, "error")) bcm_log_text(c, rr, r);
    return rr;
}
static void pl_status(void)
{
    static char r[400]; uint32_t v; int rr;
    bcm_cmd_t0 = current_tick; rr = bcm_gencmd_watched("mp_get_status", r, sizeof r, 2);
    if (rr < 0) return;
    if ((v = pl_num(r, "duration=")) != 0xffffffffu) pl_dur_ms = v;
    if ((v = pl_num(r, "elapsed="))  != 0xffffffffu) { pl_el_ms = v; vr_el_tick = current_tick; }
    v = pl_num(r, "buffering="); pl_buffering = v == 0xffffffffu ? -1 : (int)v;
}
/* the picture on screen: Disp time / seq_num from mp_get_stats video */
static int pl_disp(uint32_t *dtime, uint32_t *seq)
{
    static char r[1400]; int rr;
    bcm_cmd_t0 = current_tick; rr = bcm_gencmd_watched("mp_get_stats video", r, sizeof r, 2);
    if (rr < 0) return -1;
    if (dtime) *dtime = pl_num(r, "Disp time=");
    if (seq) *seq = pl_num(r, "Disp seq_num=");
    return 0;
}
static void fmt_time(char *b, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(b, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(b, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}
static int osd_alloc(int type, int w, int h, uint32_t *img, uint32_t *addr)
{
    uint32_t pl[8], o[4];
    memset(pl, 0, sizeof pl);
    ((uint8_t *)pl)[4] = (uint8_t)type; pl[2] = (uint32_t)w; pl[3] = (uint32_t)h; pl[4] = (uint32_t)w * (type == 15 ? 4 : 2);
    ((uint8_t *)pl)[0x1c] = 1;
    if (bcm_gfx_rpc(8, pl, 0x20, o, NULL, 300) != 0 || !o[0] || o[1] < 0x1000) return -1;
    *img = o[0]; *addr = o[1]; return 0;
}

/* ---- drawing (into the canvases) */
static void osd_draw(void)
{
    struct cv c0 = { osd_cv0, 48, 48 }, c1 = { osd_cv1, 112, 24 }, c2 = { osd_cv2, 320, 52 };
    uint32_t shown = sc_active ? pl_target_ms : pl_el_ms, rem;
    char a[16], d[20], t[8];
    int fill, k, lvl = battery_level();
    cv_clear(&c0); cv_clear(&c1); cv_clear(&c2);
    if (bcm_tv)
    {   /* v55: the LCD shows a TV while the picture is on the TV: screen 72x48 with a frame, two antennas, a stand */
        struct cv c3 = { osd_cv3, 96, 80 };
        int i;
        cv_clear(&c3);
        for (i = 0; i < 12; i++) { cv_shadowed(&c3, 36 - i, 14 - i, 2, 2, 0xFFF4F4F4); cv_shadowed(&c3, 58 + i, 14 - i, 2, 2, 0xFFF4F4F4); }
        cv_fill(&c3, 12, 16, 72, 48, 0xA0101010);
        cv_shadowed(&c3, 12, 16, 72, 3, 0xFFF4F4F4); cv_shadowed(&c3, 12, 61, 72, 3, 0xFFF4F4F4);
        cv_shadowed(&c3, 12, 16, 3, 48, 0xFFF4F4F4); cv_shadowed(&c3, 81, 16, 3, 48, 0xFFF4F4F4);
        cv_fill(&c3, 22, 26, 52, 28, 0xFF2878C8);                  /* a "picture" on its screen */
        cv_shadowed(&c3, 40, 66, 16, 3, 0xFFF4F4F4); cv_shadowed(&c3, 30, 70, 36, 3, 0xFFF4F4F4);
    }
    /* status icon */
    k = sc_active ? (sc_dir > 0 ? VUI_FF : VUI_REW) : (pl_paused ? VUI_PAUSE : VUI_PLAY);
    if (vui_state == 1 && vui[k].px) cv_blit(&c0, 8, 10, k, -1);    /* screen (16,18), centred in the stock 33x33 box */
    else
    {
        int i;
        cv_fill(&c0, 7, 7, 33, 33, 0x80181818);
        if (k == VUI_PAUSE) { cv_shadowed(&c0, 16, 14, 5, 17, 0xFFF4F4F4); cv_shadowed(&c0, 25, 14, 5, 17, 0xFFF4F4F4); }
        else if (k == VUI_PLAY) for (i = 0; i < 9; i++) cv_shadowed(&c0, 17 + i, 14 + i, 1, 17 - 2 * i, 0xFFF4F4F4);
        else for (i = 0; i < 7; i++)
        {
            int x1 = k == VUI_FF ? 13 + i : 27 - i, x2 = k == VUI_FF ? 22 + i : 18 - i;
            cv_shadowed(&c0, x1, 16 + i, 1, 13 - 2 * i, 0xFFF4F4F4); cv_shadowed(&c0, x2, 16 + i, 1, 13 - 2 * i, 0xFFF4F4F4);
        }
    }
    /* battery %, right-aligned at x 305 (mirrors the stock 15 px inset of the status icon) */
    snprintf(t, sizeof t, "%d", lvl < 0 ? 0 : lvl > 100 ? 100 : lvl);
    /* v47: battery colour: 60-100 % green, 20-59 % orange, 0-19 % red */
    cv_tint = lvl >= 60 ? 0x50E050 : lvl >= 20 ? 0xFF9A1E : 0xF03228;
    {
        int pw = 12, tw = cv_text_w(t) + pw, x = 305 - 200 - tw, r, q, lx;
        cv_text(&c1, x, 3, t);
        for (r = 0; r < 7; r++) for (q = 0; q < 5; q++)           /* '%' in the same white-with-shadow style */
            if (lk_font[12][r] & (16 >> q)) cv_shadowed(&c1, x + tw - pw + 1 + 2 * q, 3 + 2 + 2 * r, 2, 2, 0xFFF4F4F4);
        cv_tint = 0xFFFFFF;
        if (charger_inserted())
        {   /* v47.1: charging -> a small lightning bolt left of the number */
            static const uint8_t bolt[13] = { 0x0c, 0x18, 0x18, 0x30, 0x30, 0x7e, 0x0c, 0x18, 0x18, 0x30, 0x20, 0x40, 0x00 };
            int bx = x - 10;
            for (r = 0; r < 13; r++) for (q = 0; q < 8; q++)
                if (bolt[r] & (0x80 >> q)) cv_shadowed(&c1, bx + q, 3 + r, 1, 1, 0xFFFFD21E);
            x = bx;
        }
        if (button_hold())
        {   /* v50: hold switch on -> the stock lock icon (21927, 17x20) left of the rest */
            lx = x - (vui[VUI_LOCK].px ? vui[VUI_LOCK].w : 12) - 4;
            if (vui_state == 1 && vui[VUI_LOCK].px) cv_blit(&c1, lx, 1, VUI_LOCK, -1);
            else
            {   /* look-alike padlock: shackle + body */
                cv_shadowed(&c1, lx + 3, 3, 6, 2, 0xFFF4F4F4); cv_shadowed(&c1, lx + 2, 4, 2, 5, 0xFFF4F4F4);
                cv_shadowed(&c1, lx + 8, 4, 2, 5, 0xFFF4F4F4); cv_shadowed(&c1, lx, 9, 12, 9, 0xFFF4F4F4);
            }
        }
    }
    cv_tint = 0xFFFFFF;
    if (av_vol_until && TIME_BEFORE(current_tick, av_vol_until))
    {   /* v51: stock volume view 22060 instead of the progress bar: speaker, 246x24 bar, loud speaker */
        int vf = 246 * av_vol_pct / 100;
        if (vui_state == 1 && vui[VUI_VOL_FRAME].px && vui[VUI_VOL_FILL].px)
        {
            cv_blit(&c2, 13, 6, VUI_SPK_LO, -1);
            cv_blit(&c2, 37, 6, VUI_VOL_FRAME, -1);
            if (vf > 0) cv_blit(&c2, 37, 6, VUI_VOL_FILL, vf);
            cv_blit(&c2, 287, 6, VUI_SPK_HI, -1);
        }
        else
        {
            cv_fill(&c2, 37, 10, 246, 16, 0x60000000);
            cv_fill(&c2, 37, 10, 246, 2, 0xFFE8E8E8); cv_fill(&c2, 37, 24, 246, 2, 0xFFE8E8E8);
            cv_fill(&c2, 37, 10, 2, 16, 0xFFE8E8E8); cv_fill(&c2, 281, 10, 2, 16, 0xFFE8E8E8);
            if (vf > 4) cv_fill(&c2, 39, 12, vf - 4, 12, 0xE0F4F4F4);
        }
        return;
    }
    /* bottom panel: progress bar (frame, then the fill cut at the position), elapsed left, remaining right */
    if (pl_dur_ms && shown > pl_dur_ms) shown = pl_dur_ms;
    rem = pl_dur_ms > shown ? pl_dur_ms - shown : 0;
    fill = pl_dur_ms ? (int)((uint64_t)shown * 280u / pl_dur_ms) : 0;
    if (vui_state == 1)
    {
        cv_blit(&c2, 20, 6, VUI_FRAME, -1);
        if (fill > 0) cv_blit(&c2, 20, 6, VUI_FILL, fill);
    }
    else
    {
        cv_fill(&c2, 21, 7, 280, 20, 0x60000000);
        cv_fill(&c2, 20, 6, 280, 2, 0xFFE8E8E8); cv_fill(&c2, 20, 24, 280, 2, 0xFFE8E8E8);
        cv_fill(&c2, 20, 6, 2, 20, 0xFFE8E8E8); cv_fill(&c2, 298, 6, 2, 20, 0xFFE8E8E8);
        if (fill > 4) cv_fill(&c2, 22, 8, fill - 4, 16, 0xE0F4F4F4);
    }
    fmt_time(a, sizeof a, shown);
    d[0] = '-'; fmt_time(d + 1, sizeof d - 1, rem);
    cv_text(&c2, 20, 28, a);
    cv_text(&c2, 300 - cv_text_w(d), 28, d);
}
/* v56 overlay model (v55 logs: the bus moves ~1 MB/s, every RPC cost a 10 ms tick, a full present ~110-270 ms, a volume step 36 ms):
   one VC image per strip, its window stays up while the overlay is shown, and every update writes only the changed pixel spans
   of each row straight into that image (the VC composes all layers again for every frame).  Windows are added / removed only
   when the overlay appears / disappears.  On the TV the same images get a second window on display 2 (op 3 A = 2, stock
   rt 0x16982c: A = TV ? 2 : 0), which disappears after 3 s like the stock TV overlay while the iPod's own screen keeps it. */
static bool osd_bottom_vol;            /* v54: the visible bottom window shows the volume bar */
static bool osd_vol_only;              /* v53: only the volume bar is up (the wheel was turned while the overlay was hidden) */
static long osd_tv_until;              /* v56: TV overlay shown until this tick (while playing) */
static unsigned osd_bytes;             /* v56: bytes written by the last update (log) */
static bool osd_tv_want(int k)
{
    return bcm_tv && cfg_tv_osd && k != 3 && (pl_paused || sc_active || TIME_BEFORE(current_tick, osd_tv_until));
}
static void osd_tv_xy(const struct osd_strip *s, int *x, int *y)
{   /* the strip's centre scaled from the 320x240 panel to the TV raster (720x480 NTSC / 720x576 PAL, stock rt 0x1c77b8) */
    int th = global_settings.bcm_tv_signal ? 576 : 480;
    *x = (s->x * 2 + s->w) * 720 / 640 - s->w / 2;
    *y = (s->y * 2 + s->h) * th / 480 - s->h / 2;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}
static void osd_write_span(struct osd_strip *s, int y, int x0, int x1)
{
    const uint32_t *row = s->cvp + y * s->w;
    if (osd_type == 15)
    {
        bcm_io_lock(); bcm_io_write(s->addr[0] + ((unsigned)y * s->w + x0) * 4, row + x0, (unsigned)(x1 - x0 + 1) * 4); bcm_io_unlock();
    }
    else
    {   /* RGB565 fallback: composite over a dark panel colour; 32-bit aligned spans */
        int x;
        x0 &= ~1; x1 |= 1; if (x1 >= s->w) x1 = s->w - 1;
        for (x = x0; x <= x1; x++)
        {
            uint32_t p = row[x], a = p >> 24, r = ((p >> 16) & 255) * a / 255 + 20 * (255 - a) / 255,
                     g = ((p >> 8) & 255) * a / 255 + 20 * (255 - a) / 255, b = (p & 255) * a / 255 + 24 * (255 - a) / 255;
            osd_row[x - x0] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
        bcm_io_lock(); bcm_io_write(s->addr[0] + ((unsigned)y * s->w + x0) * 2, osd_row, (unsigned)(x1 - x0 + 1) * 2); bcm_io_unlock();
    }
    osd_bytes += (unsigned)(x1 - x0 + 1) * (osd_type == 15 ? 4 : 2);
}
/* full = whole image (it is not on screen yet) */
static void osd_update(struct osd_strip *s, bool full)
{
    int y;
    for (y = 0; y < s->h; y++)
    {
        const uint32_t *a = s->cvp + y * s->w;
        uint32_t *b = s->shadow + y * s->w;
        int x0 = 0, x1 = s->w - 1;
        if (!full)
        {
            while (x0 < s->w && a[x0] == b[x0]) x0++;
            if (x0 == s->w) continue;
            while (a[x1] == b[x1]) x1--;
        }
        osd_write_span(s, y, x0, x1);
        memcpy(b + x0, a + x0, (size_t)(x1 - x0 + 1) * 4);
        if (full && (y & 15) == 15) bcm_host_service();
    }
}
/* mask = strips to show with fresh content (1 icon, 2 battery, 4 bottom, 8 TV symbol) */
static void osd_present_mask(int mask)
{
    uint32_t o[4], pl[8], w4[4];
    int k, adds = 0;
    bool open = false;
    if (osd_ok <= 0) return;
    osd_bytes = 0;
    osd_draw();
    if (mask & 4) osd_bottom_vol = av_vol_until && TIME_BEFORE(current_tick, av_vol_until);
    for (k = 0; k < OSD_NS; k++)
    {
        struct osd_strip *s = &osd_s[k];
        if (!(mask & (1 << k))) continue;
        osd_update(s, !s->win[0] && !s->tvwin);
    }
    for (k = 0; k < OSD_NS; k++)
    {
        struct osd_strip *s = &osd_s[k];
        bool tv = osd_tv_want(k) && ((mask & (1 << k)) || s->win[0]);
        if (!(mask & (1 << k)) && !(s->tvwin && !tv)) continue;
        if ((mask & (1 << k)) && !s->win[0])
        {
            if (!open) { bcm_gfx_rpc(1, NULL, 0, o, NULL, 300); open = true; }
            memset(pl, 0, sizeof pl);
            pl[1] = (uint32_t)cfg_osd_layer; pl[3] = s->img[0];
            ((uint16_t *)pl)[8] = (uint16_t)s->x; ((uint16_t *)pl)[9] = (uint16_t)s->y; ((uint16_t *)pl)[12] = (uint16_t)s->w; ((uint16_t *)pl)[13] = (uint16_t)s->h;
            if (bcm_gfx_rpc(3, pl, 0x20, o, NULL, 300) == 0) s->win[0] = o[0] ? o[0] : 0xffffffffu;   /* (0 is a valid handle) */
            adds++;
        }
        if (tv && !s->tvwin)
        {
            int tx, ty;
            if (!open) { bcm_gfx_rpc(1, NULL, 0, o, NULL, 300); open = true; }
            osd_tv_xy(s, &tx, &ty);
            memset(pl, 0, sizeof pl);
            pl[0] = 2; pl[1] = (uint32_t)cfg_osd_layer; pl[3] = s->img[0];
            ((uint16_t *)pl)[8] = (uint16_t)tx; ((uint16_t *)pl)[9] = (uint16_t)ty; ((uint16_t *)pl)[12] = (uint16_t)s->w; ((uint16_t *)pl)[13] = (uint16_t)s->h;
            if (bcm_gfx_rpc(3, pl, 0x20, o, NULL, 300) == 0) s->tvwin = o[0] ? o[0] : 0xffffffffu;
            adds++;
        }
        else if (!tv && s->tvwin)
        {
            if (!open) { bcm_gfx_rpc(1, NULL, 0, o, NULL, 300); open = true; }
            memset(w4, 0, sizeof w4); w4[0] = s->tvwin == 0xffffffffu ? 0 : s->tvwin; bcm_gfx_rpc(4, w4, 0x10, o, NULL, 300); s->tvwin = 0;
        }
        s->front = 0;
    }
    if (open) bcm_gfx_rpc(2, NULL, 0, o, NULL, 300);
    osd_visible = true;
}
static void osd_present(void) { osd_vol_only = false; osd_present_mask(bcm_tv ? 15 : 7); }
static int vol_direct_n, vol_full_n;
static unsigned long vol_us_direct, vol_us_full;
static void osd_hide(void)
{
    uint32_t o[4], w4[4];
    int k;
    if (!osd_visible) return;
    bcm_gfx_rpc(1, NULL, 0, o, NULL, 300);
    for (k = 0; k < OSD_NS; k++)
    {
        struct osd_strip *s = &osd_s[k];
        if (s->win[0]) { memset(w4, 0, sizeof w4); w4[0] = s->win[0] == 0xffffffffu ? 0 : s->win[0]; bcm_gfx_rpc(4, w4, 0x10, o, NULL, 300); s->win[0] = 0; }
        if (s->tvwin) { memset(w4, 0, sizeof w4); w4[0] = s->tvwin == 0xffffffffu ? 0 : s->tvwin; bcm_gfx_rpc(4, w4, 0x10, o, NULL, 300); s->tvwin = 0; }
    }
    bcm_gfx_rpc(2, NULL, 0, o, NULL, 300);
    osd_visible = false; osd_vol_only = false; osd_bottom_vol = false;
}
static void osd_init(void)
{
    int k, ok = 1;
    vui_load();
    osd_type = cfg_osd_argb ? 15 : 1;
    for (k = 0; k < OSD_NS && ok; k++)
        ok = osd_alloc(osd_type, osd_s[k].w, osd_s[k].h, &osd_s[k].img[0], &osd_s[k].addr[0]) == 0;   /* v56: one image each */
    if (!ok && osd_type == 15)
    {
        BLOG("  OSD: 32-bit ARGB images (type 15) refused -- falling back to RGB565 on a dark panel");
        osd_type = 1; ok = 1;
        for (k = 0; k < OSD_NS && ok; k++)
            ok = osd_alloc(1, osd_s[k].w, osd_s[k].h, &osd_s[k].img[0], &osd_s[k].addr[0]) == 0;
    }
    osd_ok = ok;
    BLOG("  OSD: %d strips, VC image type %d (%s), layer %d -> %s", OSD_NS, osd_type, osd_type == 15 ? "ARGB, drawn over the video" : "RGB565",
         cfg_osd_layer, ok ? "ok" : "FAILED");
}
static void osd_show_for(int ms)
{
    if (!cfg_osd) return;
    if (osd_ok < 0) osd_init();
    if (!sc_active) pl_status();
    if (bcm_tv) osd_tv_until = current_tick + ((ms ? ms : 3000) * HZ) / 1000;   /* v56: the TV copy goes after 3 s */
    osd_present();
    osd_hide_at = ms && !bcm_tv ? current_tick + (ms * HZ) / 1000 : 0;   /* v55: on the TV the LCD keeps the controls up */
    osd_next = current_tick + HZ;
}

/* ======================================================================
   v73: soft subtitles.  apps/bcm_subs.c reads SRT / WebVTT / ASS and draws text; this part finds the tracks, keeps the
   clock and puts the text over the picture in its own ARGB VideoCore window, one layer below the controls.
   Sources (best first): sidecar files next to the video (Movie.srt, Movie.en.srt, Movie.English.forced.vtt, Movie.ass),
   then the MP4's own timed-text tracks (tx3g, as written by HandBrake / ffmpeg -c:s mov_text / iTunes).
   Hold SELECT (0.6 s) to switch: track 1 -> track 2 -> ... -> off -> track 1; a short notice names the choice.
   A short SELECT press still shows / hides the controls.  The text moves up while the controls' bottom panel is up and
   goes to the top for cues marked so ({\an8}, ASS \an7-9, WebVTT line:0%).  On a TV it is drawn on the TV, larger.
   Drawing: the canvas is rendered on the ARM; only the pixels in the rows' text spans that differ from what the back image
   holds go over the bus (a 2-line cue on the LCD: ~40 KB), then the two images swap windows in one display update, so a
   half-written subtitle is never seen. */
#define ST_TRACKS (SUBS_MAX_TRACKS + 6)
struct st_tx3g { uint32_t timescale, stts, stts_n, stsc, stsc_n, stsz, stsz_fixed, stsz_n, stco, stco_n, bytes; bool co64; };
static struct subs_track st_trk[ST_TRACKS];
static struct st_tx3g st_tx[6];
static int st_n, st_ntx;
static int st_cur = -1;                  /* the chosen track (-1 = off) */
static int st_loaded = -1;               /* the track in st_store */
static int st_store_h = -1;
static struct subs_store st_store;
static char st_video[MAX_PATH];
static int st_ok;                        /* drawing: 0 = not set up yet, 1 = ready, -1 = failed (no subtitles this run) */
static int st_cv_h = -1;
static uint32_t *st_cv;                  /* ARGB canvas st_w x st_h */
static uint8_t *st_wk;                   /* alpha work buffer */
static int16_t *st_ext;                  /* [2][st_h][2]: per image and row the columns that are not transparent */
static int st_w, st_h, st_tvmode;
static uint32_t st_img[2], st_addr[2], st_win;
static int st_nimg, st_front = -1;
static int st_font = -1;                 /* font slot loaded for the subtitles (-1 = none: the UI font) */
static int st_font_h;
static struct subs_style st_style;
static const struct subs_cue *st_shown[6];
static int st_nshown, st_shown_msgv = -1, st_msgv;
static bool st_shown_any, st_win_top, st_win_raised;
static char st_msg[64];
static long st_msg_until, st_fast_until, st_sync_tick;
static uint32_t st_last_el;
static bool st_stalled;                  /* the last two position reads were equal while playing (buffering) */
static bool st_sel_down, st_sel_done;
static long st_sel_t0;

/* ---- tracks */
static const char *st_lang_name(const char *c)
{
    static const char *const map[][2] = {
        { "eng", "English" }, { "spa", "Spanish" }, { "fre", "French" }, { "fra", "French" }, { "ger", "German" },
        { "deu", "German" }, { "ita", "Italian" }, { "por", "Portuguese" }, { "jpn", "Japanese" }, { "chi", "Chinese" },
        { "zho", "Chinese" }, { "kor", "Korean" }, { "rus", "Russian" }, { "dut", "Dutch" }, { "nld", "Dutch" },
        { "swe", "Swedish" }, { "nor", "Norwegian" }, { "dan", "Danish" }, { "fin", "Finnish" }, { "pol", "Polish" },
        { "ara", "Arabic" }, { "heb", "Hebrew" }, { "hin", "Hindi" }, { "tur", "Turkish" }, { "gre", "Greek" },
        { "ell", "Greek" }, { "cze", "Czech" }, { "ces", "Czech" }, { "hun", "Hungarian" }, { "ukr", "Ukrainian" } };
    unsigned i;
    for (i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcmp(c, map[i][0])) return map[i][1];
    return NULL;
}
/* an MP4 timed-text track: handler 'text' / 'sbtl' / 'subt' with a 'tx3g' sample description */
static bool st_tx3g_trak(uint32_t ps, uint32_t pe, struct st_tx3g *x, char *label, size_t ln)
{
    uint32_t ms, me, a, b, mi_s, mi_e, sb_s, sb_e, pos, t, s, e, h, lang, flags = 0;
    bool have_stsd = false;
    char code[4];
    memset(x, 0, sizeof *x);
    if (!m4_find(ps, pe, FOURCC('m','d','i','a'), &ms, &me)) return false;
    if (!m4_find(ms, me, FOURCC('h','d','l','r'), &a, &b)) return false;
    h = m4_u32(a + 8);
    if (h != FOURCC('t','e','x','t') && h != FOURCC('s','b','t','l') && h != FOURCC('s','u','b','t')) return false;
    if (!m4_find(ms, me, FOURCC('m','d','h','d'), &a, &b)) return false;
    x->timescale = m4_u8(a) == 1 ? m4_u32(a + 20) : m4_u32(a + 12);
    lang = m4_u16(a + (m4_u8(a) == 1 ? 32 : 20));
    if (!x->timescale) return false;
    if (!m4_find(ms, me, FOURCC('m','i','n','f'), &mi_s, &mi_e)) return false;
    if (!m4_find(mi_s, mi_e, FOURCC('s','t','b','l'), &sb_s, &sb_e)) return false;
    pos = sb_s;
    while (m4_box(&pos, sb_e, &t, &s, &e))
    {
        if (t == FOURCC('s','t','s','d'))
        {
            if (m4_u32(s + 8 + 4) != FOURCC('t','x','3','g')) return false;
            flags = m4_u32(s + 8 + 16);                  /* displayFlags: 0x80000000 all samples forced, 0x40000000 some */
            have_stsd = true;
        }
        else if (t == FOURCC('s','t','t','s')) { x->stts = s + 8; x->stts_n = m4_u32(s + 4); }
        else if (t == FOURCC('s','t','s','c')) { x->stsc = s + 8; x->stsc_n = m4_u32(s + 4); }
        else if (t == FOURCC('s','t','s','z')) { x->stsz_fixed = m4_u32(s + 4); x->stsz_n = m4_u32(s + 8); x->stsz = s + 12; }
        else if (t == FOURCC('s','t','c','o')) { x->stco = s + 8; x->stco_n = m4_u32(s + 4); x->co64 = false; }
        else if (t == FOURCC('c','o','6','4')) { x->stco = s + 8; x->stco_n = m4_u32(s + 4); x->co64 = true; }
    }
    if (!have_stsd || !x->stts_n || !x->stsc_n || !x->stsz_n || !x->stco_n) return false;
    {   uint32_t i;
        if (x->stsz_fixed) x->bytes = x->stsz_fixed * x->stsz_n;
        else for (i = 0; i < x->stsz_n && x->bytes < 8u << 20; i++) x->bytes += m4_u32(x->stsz + 4 * i);
    }
    code[0] = (char)(((lang >> 10) & 31) + 0x60); code[1] = (char)(((lang >> 5) & 31) + 0x60); code[2] = (char)((lang & 31) + 0x60); code[3] = 0;
    {
        const char *nm = st_lang_name(code);
        bool known = code[0] >= 'a' && code[0] <= 'z' && strcmp(code, "und");
        snprintf(label, ln, "%s%s (MP4)", nm ? nm : known ? code : "Text", flags & 0x80000000u ? " forced" : "");
    }
    return true;
}
static void st_find_tracks(const char *video)
{
    uint32_t pos, t, ps, pe;
    snprintf(st_video, sizeof st_video, "%s", video);
    st_n = subs_find_sidecars(video, st_trk, SUBS_MAX_TRACKS);
    st_ntx = 0;
    if (m4.fd >= 0 && m4_moov_e)
    {
        pos = m4_moov_s;
        while (st_ntx < (int)(sizeof st_tx / sizeof st_tx[0]) && st_n < ST_TRACKS && m4_box(&pos, m4_moov_e, &t, &ps, &pe))
            if (t == FOURCC('t','r','a','k') && st_tx3g_trak(ps, pe, &st_tx[st_ntx], st_trk[st_n].label, sizeof st_trk[st_n].label))
            {
                st_trk[st_n].kind = SUBS_TX3G; st_trk[st_n].index = (uint8_t)st_ntx; st_trk[st_n].path[0] = 0;
                st_ntx++; st_n++;
            }
    }
    BLOG("subtitles: %d track(s)%s", st_n, st_n ? ":" : " (none next to the video, none inside it)");
    {   int k;
        for (k = 0; k < st_n; k++)
            BLOG("  %d: %s  %s", k + 1, st_trk[k].label, st_trk[k].kind == SUBS_TX3G ? "(MP4 timed text)" : st_trk[k].path);
    }
}
static void st_free_store(void)
{
    if (st_store_h > 0) { core_unpin(st_store_h); core_free(st_store_h); }
    st_store_h = -1; st_loaded = -1;
    memset(&st_store, 0, sizeof st_store);
}
/* the samples of a tx3g track: u16 length + UTF-8 (or UTF-16 with a byte-order mark) text, then optional boxes:
   'styl' runs (italic kept). */
static int st_load_tx3g(const struct st_tx3g *x)
{
    static uint8_t sb[2048];
    static char txt[3200];
    uint32_t k, chunk = 1, s_in_chunk = 0, off_in_chunk = 0, stsc_i = 0, spc, next_first, stts_i = 0, stts_left, delta;
    uint64_t dts = 0;
    int fd = open(st_video, O_RDONLY), added = 0;
    if (fd < 0) return -1;
    spc = m4_u32(x->stsc + 4); if (!spc) spc = 1;
    next_first = x->stsc_n > 1 ? m4_u32(x->stsc + 12) : 0xffffffffu;
    stts_left = m4_u32(x->stts); delta = m4_u32(x->stts + 4);
    for (k = 0; k < x->stsz_n && chunk <= x->stco_n; k++)
    {
        uint32_t size = x->stsz_fixed ? x->stsz_fixed : m4_u32(x->stsz + 4 * k);
        uint32_t coff = x->co64 ? m4_u32(x->stco + 8 * (chunk - 1) + 4) : m4_u32(x->stco + 4 * (chunk - 1));
        uint32_t off = coff + off_in_chunk, start = (uint32_t)(dts * 1000ULL / x->timescale);
        uint32_t end = (uint32_t)((dts + delta) * 1000ULL / x->timescale);
        /* advance the cursors */
        off_in_chunk += size;
        if (++s_in_chunk >= spc)
        {
            chunk++; s_in_chunk = 0; off_in_chunk = 0;
            if (chunk >= next_first && stsc_i + 1 < x->stsc_n)
            {
                stsc_i++; spc = m4_u32(x->stsc + 12 * stsc_i + 4); if (!spc) spc = 1;
                next_first = stsc_i + 1 < x->stsc_n ? m4_u32(x->stsc + 12 * (stsc_i + 1)) : 0xffffffffu;
            }
        }
        dts += delta;
        if (stts_left && --stts_left == 0 && stts_i + 1 < x->stts_n)
        {
            stts_i++; stts_left = m4_u32(x->stts + 8 * stts_i); delta = m4_u32(x->stts + 8 * stts_i + 4);
        }
        if ((k & 31) == 31) bcm_host_service();          /* the picture keeps being fed meanwhile */
        if (size < 3) continue;                           /* an empty sample = nothing shown (a gap) */
        if (size > sizeof sb) size = sizeof sb;
        if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off || read(fd, sb, size) != (ssize_t)size) continue;
        {
            uint32_t tl = (uint32_t)sb[0] << 8 | sb[1], i, o = 0, q;
            uint32_t ist[16], ien[16];                    /* italic runs, in characters */
            int nit = 0;
            bool u16 = false;
            if (tl > size - 2) tl = size - 2;
            /* modifier boxes after the text */
            for (q = 2 + tl; q + 8 <= size; )
            {
                uint32_t bs = (uint32_t)sb[q] << 24 | (uint32_t)sb[q + 1] << 16 | (uint32_t)sb[q + 2] << 8 | sb[q + 3];
                if (bs < 8 || q + bs > size) break;
                if (!memcmp(sb + q + 4, "styl", 4) && bs >= 10)
                {
                    uint32_t ne = (uint32_t)sb[q + 8] << 8 | sb[q + 9], r;
                    for (r = 0; r < ne && 10 + 12 * (r + 1) <= bs && nit < 16; r++)
                    {
                        const uint8_t *en = sb + q + 10 + 12 * r;
                        if (en[6] & 2) { ist[nit] = (uint32_t)en[0] << 8 | en[1]; ien[nit] = (uint32_t)en[2] << 8 | en[3]; nit++; }
                    }
                }
                q += bs;
            }
            if (tl >= 2 && sb[2] == 0xFE && sb[3] == 0xFF) u16 = true;
            /* text: '<' and '&' escaped (subs_add reads HTML-style tags), italic runs as style codes */
            {
                uint32_t ch = 0;                          /* character index */
                bool it = false;
                const uint8_t *p = sb + 2 + (u16 ? 2 : 0), *pe = sb + 2 + tl;
                while (p < pe && o + 12 < sizeof txt)
                {
                    ucschar_t c;
                    bool want = false;
                    int r;
                    for (r = 0; r < nit; r++) if (ch >= ist[r] && ch < ien[r]) want = true;
                    if (want != it) { txt[o++] = want ? SUBS_C_ITALIC_ON : SUBS_C_ITALIC_OFF; it = want; }
                    if (u16)
                    {
                        if (p + 1 >= pe) break;
                        c = (ucschar_t)(p[0] << 8 | p[1]); p += 2;
                        if (c >= 0xD800 && c < 0xDC00 && p + 1 < pe) { c = 0x10000 + ((c - 0xD800) << 10) + ((p[0] << 8 | p[1]) - 0xDC00); p += 2; }
                    }
                    else p = utf8decode(p, &c);
                    if (c == '<') { memcpy(txt + o, "&lt;", 4); o += 4; }
                    else if (c == '&') { memcpy(txt + o, "&amp;", 5); o += 5; }
                    else o = (uint32_t)((char *)utf8encode(c, (unsigned char *)txt + o) - txt);
                    ch++;
                }
                if (it) txt[o++] = SUBS_C_ITALIC_OFF;
            }
            i = o;
            if (!subs_add(&st_store, start, end, txt, i, 0)) break;      /* buffer full: keep what fits */
            added++;
        }
    }
    close(fd);
    subs_finish(&st_store);
    return (int)st_store.n;
}
static int st_load(int k)
{
    const struct subs_track *t = &st_trk[k];
    uint32_t want;
    int n;
    long t0 = current_tick;
    st_free_store();
    if (t->kind == SUBS_TX3G)
    {
        const struct st_tx3g *x = &st_tx[t->index];
        want = x->bytes * 3 + x->stsz_n * 24 + 4096;
    }
    else
    {
        int fd = open(t->path, O_RDONLY);
        long fs;
        if (fd < 0) { BLOG("subtitles: cannot open %s", t->path); return -1; }
        fs = ffilesize(fd); close(fd);
        if (fs <= 0 || fs > 8L << 20) { BLOG("subtitles: %s: size %ld -- not loaded", t->path, fs); return -1; }
        want = subs_buffer_size((uint32_t)fs);
    }
    st_store_h = core_alloc_ex(want, NULL);
    if (st_store_h <= 0) { st_store_h = -1; BLOG("subtitles: no memory (%u KB)", (unsigned)(want / 1024)); return -1; }
    core_pin(st_store_h);
    subs_store_init(&st_store, core_get_data(st_store_h), want);
    n = t->kind == SUBS_TX3G ? st_load_tx3g(&st_tx[t->index]) : subs_load_file(&st_store, t->path, t->kind);
    BLOG("subtitles: '%s' -> %d cues (%u of %u KB used, %ld ms)", t->label, n, (unsigned)(st_store.used / 1024),
         (unsigned)(want / 1024), (current_tick - t0) * 1000 / HZ);
    if (n <= 0) { st_free_store(); return -1; }
    st_loaded = k;
    return n;
}
static void st_toast(const char *text)
{   /* v74: any short notice in the subtitle window (it is set up for it if needed) */
    if (st_ok < 0) return;
    snprintf(st_msg, sizeof st_msg, "%s", text);
    st_msg_until = current_tick + 2 * HZ; st_msgv++;
}
static void st_notice(const char *label)
{
    snprintf(st_msg, sizeof st_msg, "%s: %s", (const char *)str(LANG_SUBTITLES), label);
    st_msg_until = current_tick + 2 * HZ; st_msgv++;
}
static void st_select(int k, bool notice)
{
    st_cur = k;
    if (k >= 0 && st_loaded != k && st_load(k) < 0)
    {
        if (notice) { char b[56]; snprintf(b, sizeof b, "%s (?)", st_trk[k].label); st_notice(b); }
        st_cur = -1;
        return;
    }
    if (notice) st_notice(k >= 0 ? st_trk[k].label : (const char *)str(LANG_OFF));
}
static void st_seeked(void) { st_fast_until = current_tick + 3 * HZ; st_stalled = false; st_last_el = 0xffffffffu; st_sync_tick = current_tick - HZ; }

/* ---- font: a sans-serif bold font near the wanted height from FONT_DIR (the overlay's build puts 21/27/35 px
   Helvetica Bold there); the UI font when there is none */
static int st_font_score(const char *nm, int want)
{
    int h = atoi(nm), d = h > want ? h - want : want - h, pref = 6;
    size_t l = strlen(nm);
    if (h <= 0 || l < 5 || strcasecmp(nm + l - 4, ".fnt")) return -1;
    if (strstr(nm, "Helvetica-Bold")) pref = 0;
    else if (strstr(nm, "Helvetica")) pref = 1;
    else if (strstr(nm, "Nimbus")) pref = 2;
    else if (strstr(nm, "Terminus-Bold")) pref = 3;
    else if (strstr(nm, "Unifont") || strstr(nm, "WenQ") || strstr(nm, "Sazanami")) pref = 5;   /* big CJK fonts: last */
    return d * 8 + pref;
}
static void st_font_open(int want)
{
    char best[MAX_PATH] = "";
    int bs = 0x7fffffff;
    DIR *d = opendir(FONT_DIR);
    struct dirent *de;
    st_font = -1;
    if (d)
    {
        while ((de = readdir(d)) != NULL)
        {
            int sc = st_font_score((const char *)de->d_name, want);
            if (sc >= 0 && sc < bs) { bs = sc; snprintf(best, sizeof best, FONT_DIR "/%s", de->d_name); }
        }
        closedir(d);
    }
    if (best[0])
    {
        int fd = open(best, O_RDONLY);
        long fs = fd >= 0 ? ffilesize(fd) : 0;
        if (fd >= 0) close(fd);
        /* whole font in RAM when it is small (glyph look-ups without disk reads, missing glyphs can be told) */
        st_font = fs > 0 && fs < (long)bcm_memprof()->font_kb * 1024 ?
                  font_load_ex(best, 0, probed_ramsize == 64 ? 65000 : 30000) : font_load(best);   /* v79: per RAM size */
        if (st_font < 0) st_font = -1;
    }
    st_font_h = font_get(st_font >= 0 ? st_font : FONT_UI)->height;
    BLOG("subtitles: font %s (%d px, wanted %d)", st_font >= 0 ? best : "= the UI font", st_font_h, want);
}
static int st_fw(void *ctx, uint32_t c) { return font_get_width(font_get((int)(intptr_t)ctx), (ucschar_t)c); }
static const uint8_t *st_fb(void *ctx, uint32_t c) { return font_get_bits(font_get((int)(intptr_t)ctx), (ucschar_t)c); }
static bool st_has_glyph(struct font *pf, ucschar_t c)
{
    if (c < pf->firstchar || c >= pf->firstchar + (unsigned)pf->size) return false;
    if (pf->fd >= 0 || (int)c == pf->defaultchar) return true;      /* glyph cache: cannot tell cheaply, assume present */
    return font_get_bits(pf, c) != font_get_bits(pf, (ucschar_t)pf->defaultchar);
}
/* the subtitle font, or the UI font when the text has characters only the UI font has (Cyrillic, CJK ...) */
static int st_pick_font(const struct subs_cue **c, int n, const char *msg)
{
    struct font *pf, *ui;
    int k;
    if (st_font < 0) return FONT_UI;
    pf = font_get(st_font); ui = font_get(FONT_UI);
    if (pf == ui) return st_font;
    for (k = 0; k < (msg ? 1 : n); k++)
    {
        const unsigned char *p = (const unsigned char *)(msg ? msg : subs_text(c[k])), *e = p + (msg ? strlen(msg) : c[k]->len);
        while (p < e)
        {
            ucschar_t ch;
            if (*p == SUBS_C_COLOR) { p += 4; continue; }
            if (*p < 0x20) { p++; continue; }
            p = utf8decode(p, &ch);
            if (ch > 0x7e && ch != 0xa0 && !st_has_glyph(pf, ch) && st_has_glyph(ui, ch)) return FONT_UI;
        }
    }
    return st_font;
}

/* ---- the window */
static void st_end(void)
{   /* the end of a video: Rockbox memory back (the VideoCore's images go with its reboot) */
    st_free_store();
    if (st_cv_h > 0) { core_unpin(st_cv_h); core_free(st_cv_h); }
    st_cv_h = -1; st_cv = NULL; st_wk = NULL; st_ext = NULL;
    if (st_font >= 0) font_unload(st_font);
    st_font = -1;
    st_ok = 0; st_win = 0; st_front = -1; st_nimg = 0; st_nshown = 0; st_shown_any = false; st_shown_msgv = -1;
    st_msg_until = 0; st_sel_down = false;
}
static void st_write(int im);
static int st_setup(void)
{
    static const int lcd_px[3] = { 15, 21, 27 }, tv_px[3] = { 27, 35, 35 };
    int sz = global_settings.bcm_sub_size, k, y;
    size_t need;
    if (st_ok) return st_ok;
    st_ok = -1;
    if (sz < 0 || sz > 2) sz = 1;
    st_tvmode = bcm_tv;
    st_w = st_tvmode ? 704 : 320;
    st_h = st_tvmode ? 176 : 144;
    st_font_open(st_tvmode ? tv_px[sz] : lcd_px[sz]);
    if (st_font_h * 2 + 12 > st_h) st_h = (st_font_h * 2 + 12 + 15) & ~15;
    need = (size_t)st_w * st_h * 5 + (size_t)st_h * 2 * 2 * sizeof(int16_t) + 64;
    st_cv_h = core_alloc_ex(need, NULL);
    if (st_cv_h <= 0) { st_cv_h = -1; BLOG("subtitles: no memory for the canvas (%u KB)", (unsigned)(need / 1024)); return -1; }
    core_pin(st_cv_h);
    st_cv = core_get_data(st_cv_h);
    st_wk = (uint8_t *)(st_cv + st_w * st_h);
    st_ext = (int16_t *)(((uintptr_t)(st_wk + st_w * st_h) + 3) & ~(uintptr_t)3);
    for (k = 0; k < 2; k++)                               /* a new image's contents are unknown: all of it gets written once */
        for (y = 0; y < st_h; y++) { st_ext[(k * st_h + y) * 2] = 0; st_ext[(k * st_h + y) * 2 + 1] = (int16_t)(st_w - 1); }
    st_nimg = 0;
    for (k = 0; k < 2; k++)
        if (osd_alloc(15, st_w, st_h, &st_img[k], &st_addr[k]) == 0) st_nimg++;
        else break;
    if (!st_nimg) { BLOG("subtitles: the VideoCore refused a %dx%d ARGB image -- no subtitles", st_w, st_h); return -1; }
    st_style.scale = 1;
    st_style.box = global_settings.bcm_sub_style == 1;
    st_style.outline = st_font_h >= 30 ? 3 : st_font_h >= 18 ? 2 : 1;
    st_style.color = 0xFFFFFF;
    st_style.margin = st_tvmode ? 28 : 6;
    st_win = 0; st_front = -1;
    memset(st_cv, 0, (size_t)st_w * st_h * 4);
    for (k = 0; k < st_nimg; k++) st_write(k);            /* clear: a new image's contents are unknown (before the picture runs) */
    st_ok = 1;
    BLOG("subtitles: %s window %dx%d, %d image(s), layer %d, %s", st_tvmode ? "TV" : "LCD", st_w, st_h, st_nimg,
         cfg_subs_layer ? cfg_subs_layer : cfg_osd_layer - 1, st_style.box ? "box" : "outline");
    return 1;
}
static void st_rm_win(uint32_t w)
{
    uint32_t o[4], w4[4] = { w == 0xffffffffu ? 0 : w, 0, 0, 0 };
    bcm_gfx_rpc(4, w4, 0x10, o, NULL, 300);
}
static void st_hide(void)
{
    uint32_t o[4];
    if (!st_win) return;
    bcm_gfx_rpc(1, NULL, 0, o, NULL, 300);
    st_rm_win(st_win);
    bcm_gfx_rpc(2, NULL, 0, o, NULL, 300);
    st_win = 0;
}
/* the image's top-left corner on the display: text block bottom at 'bottom - 2' inside the image */
static void st_xy(bool top, bool raised, int *x, int *y)
{
    if (st_tvmode)
    {
        int th = global_settings.bcm_tv_signal ? 576 : 480;
        *x = (720 - st_w) / 2;
        if (top) *y = th / 16 - 2;
        else
        {
            int b = th - th / 16;                         /* clear of the overscan */
            if (raised) { int tx, ty; osd_tv_xy(&osd_s[2], &tx, &ty); if (ty - 4 < b) b = ty - 4; }
            *y = b - (st_h - 2);
        }
    }
    else
    {
        *x = 0;
        *y = top ? 0 : (raised ? osd_s[2].y - 2 : LCD_HEIGHT) - st_h;
    }
    if (*y < 0) *y = 0;
}
static void st_place(int im, bool top, bool raised)
{
    uint32_t o[4], pl[8], old = st_win;
    int x, y;
    st_xy(top, raised, &x, &y);
    bcm_gfx_rpc(1, NULL, 0, o, NULL, 300);
    memset(pl, 0, sizeof pl);
    pl[0] = st_tvmode ? 2 : 0;
    pl[1] = (uint32_t)(cfg_subs_layer ? cfg_subs_layer : cfg_osd_layer - 1);
    pl[3] = st_img[im];
    ((uint16_t *)pl)[8] = (uint16_t)x; ((uint16_t *)pl)[9] = (uint16_t)y;
    ((uint16_t *)pl)[12] = (uint16_t)st_w; ((uint16_t *)pl)[13] = (uint16_t)st_h;
    st_win = bcm_gfx_rpc(3, pl, 0x20, o, NULL, 300) == 0 ? (o[0] ? o[0] : 0xffffffffu) : 0;
    if (old) st_rm_win(old);
    bcm_gfx_rpc(2, NULL, 0, o, NULL, 300);
    st_front = im; st_win_top = top; st_win_raised = raised;
}
/* write the canvas into image im: per row, the union of the old and the new non-transparent columns */
static void st_write(int im)
{
    int y, x;
    for (y = 0; y < st_h; y++)
    {
        const uint32_t *row = st_cv + y * st_w;
        int16_t *ex = st_ext + (im * st_h + y) * 2;
        int n0 = st_w, n1 = -1, x0, x1;
        for (x = 0; x < st_w; x++) if (row[x] >> 24) { n0 = x; break; }
        if (n0 < st_w) for (x = st_w - 1; x >= n0; x--) if (row[x] >> 24) { n1 = x; break; }
        x0 = n0 < ex[0] ? n0 : ex[0];
        x1 = n1 > ex[1] ? n1 : ex[1];
        if (x1 >= x0)
        {
            bcm_io_lock(); bcm_io_write(st_addr[im] + ((unsigned)y * st_w + x0) * 4, row + x0, (unsigned)(x1 - x0 + 1) * 4); bcm_io_unlock();
        }
        ex[0] = (int16_t)(n1 >= 0 ? n0 : st_w); ex[1] = (int16_t)(n1 >= 0 ? n1 : -1);
        if ((y & 15) == 15) bcm_host_service();
    }
}
static uint32_t st_now(void)
{
    int32_t pos = (int32_t)pl_el_ms;
    if (!pl_paused && !st_stalled && vr_el_tick) pos += (int32_t)(current_tick - vr_el_tick) * (1000 / HZ);
    pos += cfg_subs_delay;
    return pos < 0 ? 0 : (uint32_t)pos;
}
/* called from the player loop (bcm_osd_tick) */
static void st_tick(void)
{
    const struct subs_cue *c[6];
    int n = 0;
    bool msg, top = false, raised;
    if (st_ok < 0) return;
    msg = st_msg_until && TIME_BEFORE(current_tick, st_msg_until);
    if (!msg) st_msg_until = 0;
    if (!msg && (st_cur < 0 || st_loaded != st_cur || sc_active))
    {   /* off, or scrubbing (the text would be stale) */
        if (st_shown_any) { st_hide(); st_shown_any = false; st_nshown = 0; st_shown_msgv = -1; }
        return;
    }
    if (!msg)
    {
        long every = TIME_BEFORE(current_tick, st_fast_until) ? HZ * 4 / 10 : (cfg_subs_sync * HZ) / 1000;
        if (!pl_paused && current_tick - st_sync_tick >= HZ * 4 / 10 && (!vr_el_tick || current_tick - vr_el_tick >= every))
        {   /* read the position; while it does not move (start-up, buffering after a seek) the clock is not run on */
            st_sync_tick = current_tick;
            pl_status();
            st_stalled = vr_el_tick && pl_el_ms == st_last_el;
            st_last_el = pl_el_ms;
        }
        n = subs_active(&st_store, st_now(), c, 6);
        if (n)
        {   int k; top = true;
            for (k = 0; k < n; k++) if (!(c[k]->flags & SUBS_TOP)) top = false;
        }
    }
    if (!msg && !n)
    {
        if (st_shown_any) { st_hide(); st_shown_any = false; st_nshown = 0; st_shown_msgv = -1; }
        return;
    }
    if (st_setup() < 0) return;
    raised = !top && (st_tvmode ? osd_s[2].tvwin != 0 : (osd_visible && osd_s[2].win[0] != 0));
    if (!st_shown_any || (msg ? st_shown_msgv != st_msgv : (st_shown_msgv >= 0 || n != st_nshown || memcmp(c, st_shown, n * sizeof c[0]))))
    {   /* new content: draw, write the back image, swap */
        struct subs_font f;
        int y0, y1, im = st_nimg == 2 ? (st_front == 0 ? 1 : 0) : 0, fid = st_pick_font(c, n, msg ? st_msg : NULL);
        struct font *pf = font_get(fid);
        unsigned long t0 = USEC_TIMER;
        f.height = pf->height; f.ascent = pf->ascent; f.depth = pf->depth;
        f.width = st_fw; f.bits = st_fb; f.ctx = (void *)(intptr_t)fid;
        subs_render(st_cv, st_wk, st_w, st_h, &f, &st_style, c, n, msg ? st_msg : NULL, 2, st_h - 2, &y0, &y1);
        st_write(im);
        if (st_nimg == 2 || !st_win || st_win_top != top || st_win_raised != raised) st_place(im, top, raised);
        st_shown_any = true; st_nshown = msg ? 0 : n; st_shown_msgv = msg ? st_msgv : -1;
        if (!msg) memcpy(st_shown, c, (size_t)n * sizeof c[0]);
        if (st_msgv < 3 || msg) BLOG("  subtitles: %s at %u ms, %d cue(s), rows %d-%d, %lu us", msg ? st_msg : "cue",
                                     (unsigned)st_now(), n, y0, y1, (unsigned long)(USEC_TIMER - t0));
    }
    else if (st_win_top != top || st_win_raised != raised || !st_win)
        st_place(st_front < 0 ? 0 : st_front, top, raised);   /* the controls came / went: move up / down */
}
/* the start of a video: tracks, the first choice (Settings > Subtitles) */
static void st_begin(const char *video)
{
    bool on = cfg_subs >= 0 ? cfg_subs != 0 : global_settings.bcm_subs;
    st_end();
    st_cur = -1;
    st_find_tracks(video);
    if (on && st_n) st_select(0, false);
    /* v78: the VideoCore images (and their first full write, 0.3 s on the LCD, ~1 s for the TV) are made once the picture
       runs (bcm_video_run, or the first cue) instead of before mp_selectplay: off the start-up path.  This part needs no
       VideoCore and runs in the start-up helper thread, while the VideoCore boots. */
}
/* hold SELECT: next track / off */
static void st_cycle(void)
{
    int k = st_cur + 1;
    if (k >= st_n) k = -1;
    st_select(k, true);
    if (st_cur < 0 && k >= 0 && k + 1 < st_n) { /* unreadable: the notice says so; the next hold tries the next one */ st_cur = k; st_loaded = -1; }
}

/* ---- key frames */
static uint32_t pl_key_ms(uint32_t k) { return (uint32_t)(m4_dts_of(k) * 1000ULL / m4.timescale); }
static uint32_t pl_key_after(uint32_t ms)
{
    uint32_t s = m4_sample_at_ms(ms), lo = 0, hi;
    if (!m4.stss || !m4.stss_n) return s + 1 < m4.stsz_n ? s + 1 : s;
    hi = m4.stss_n;                                   /* first sync sample number (1-based) > s+1 */
    if (m4_u32(m4.stss + 4 * (hi - 1)) <= s + 1) return 0xffffffffu;
    while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (m4_u32(m4.stss + 4 * mid) <= s + 1) lo = mid + 1; else hi = mid; }
    return m4_u32(m4.stss + 4 * lo) - 1;
}
static uint32_t pl_key_step(uint32_t cur, int dir, uint32_t jump)
{
    uint32_t kms = cur, k;
    if (dir > 0)
    {
        uint32_t want = cur + jump;
        if (pl_dur_ms && want > pl_dur_ms) want = pl_dur_ms;
        m4_key_at(want, &kms);
        if (kms <= cur) { k = pl_key_after(cur); if (k != 0xffffffffu) kms = pl_key_ms(k); else kms = cur; }
    }
    else
    {
        uint32_t want = cur > jump ? cur - jump : 0;
        if (want >= cur && cur) want = cur - 1;
        m4_key_at(want, &kms);
    }
    return kms;
}
/* the trick feed (tf_*, used in v_get_frame): next key frame in direction tf_dir, tf_jump ms on */
static uint32_t tf_next_key(void)
{
    uint32_t k = pl_key_step(tf_cur, tf_dir, tf_jump);
    if (!tf_fed) { uint32_t k0; m4_key_at(tf_cur, &k0); k = k0; }  /* the first frame: the key frame at the start position */
    return k;
}

static void mon_start(const char *why)
{
    mon_t0 = current_tick; mon_until = current_tick + cfg_seekmon * HZ; mon_next = current_tick + HZ / 2; mon_req0 = v_getframes;
    BLOG("  mon: %s -- watching elapsed/buffering for %d s", why, cfg_seekmon);
}
static void mon_tick(void)
{
    static char r[400];
    uint32_t el, bu; int rr;
    if (!mon_until || TIME_BEFORE(current_tick, mon_next)) return;
    if (!TIME_BEFORE(current_tick, mon_until)) { mon_until = 0; return; }
    mon_next = current_tick + HZ / 2;
    bcm_cmd_t0 = current_tick; rr = bcm_gencmd_watched("mp_get_status", r, sizeof r, 2);
    if (rr < 0) return;
    el = pl_num(r, "elapsed="); bu = pl_num(r, "buffering=");
    if (el != 0xffffffffu) pl_el_ms = el;
    BLOG("  mon +%ld.%lds: elapsed=%u buffering=%d paused=%s frames requested since=%d",
         (current_tick - mon_t0) / HZ, (current_tick - mon_t0) % HZ / 10, (unsigned)el, bu == 0xffffffffu ? -1 : (int)bu,
         strstr(r, "paused=yes") ? "yes" : "no", v_getframes - mon_req0);
}

/* ---- trick probe: measure which commands really move the picture (runs once, buttons ignored) */
struct tp_res { int dseq, dtime; };
static struct tp_res tp_r[6];
static void tp_sample(const char *name, long t0, uint32_t *seq, uint32_t *dtime)
{
    static char r[1400], s[400];
    bcm_cmd_t0 = current_tick; bcm_gencmd_watched("mp_get_stats video", r, sizeof r, 2);
    bcm_cmd_t0 = current_tick; bcm_gencmd_watched("mp_get_status", s, sizeof s, 2);
    *seq = pl_num(r, "Disp seq_num="); *dtime = pl_num(r, "Disp time=");
    {   const char *m = strstr(s, "mode="); char mb[10] = "?"; int i = 0;
        if (m) { m += 5; while (*m && *m != ' ' && i < 9) mb[i++] = *m++; mb[i] = 0; }
        BLOG("  probe %-11s +%ld.%02lds  seq=%d disp_time=%d elapsed=%d mode=%s buffering=%d fed=%d porder=%d",
             name, (current_tick - t0) / HZ, (current_tick - t0) % HZ, (int)*seq, (int)*dtime, (int)pl_num(s, "elapsed="), mb,
             (int)pl_num(s, "buffering="), v_getframes, (int)pl_num(r, "lost_frame_assert_packetorder="));
    }
}
static void tp_wait_ms(long ms)
{
    long end = current_tick + (ms * HZ) / 1000;
    while (TIME_BEFORE(current_tick, end)) { bcm_host_service(); yield(); }
}
/* resume normal play at `pos` and wait (<= 4 s) until the picture moves again */
static void tp_recover(uint32_t pos)
{
    char c[40]; uint32_t k, s0 = 0, s1 = 0, t; long end;
    tf_dir = 0;
    pl_cmd("mp_play", false);
    m4_key_at(pos, &k);
    snprintf(c, sizeof c, "mp_seek %u", (unsigned)k); pl_cmd(c, false);
    pl_disp(&t, &s0); s1 = s0;
    for (end = current_tick + 4 * HZ; TIME_BEFORE(current_tick, end); )
    {
        tp_wait_ms(250);
        if (pl_disp(&t, &s1) == 0 && s1 != 0xffffffffu && s1 > s0 + 2 && t >= k) break;
    }
    BLOG("  probe: resumed at %u ms (picture moving: %s)", (unsigned)k, s1 > s0 + 2 ? "yes" : "no");
}
static void tp_test(int idx, const char *name, const char *start, const char *rep, int rep_ms, int feed, uint32_t pos)
{
    uint32_t q0, t0, q1, t1, q, t; long ts = current_tick, next_rep, next_s, end = current_tick + (cfg_probe_ms * HZ) / 1000;
    BLOG("  probe %s: start=\"%s\" repeat=\"%s\"/%d ms, feed=%s", name, start ? start : "", rep ? rep : "", rep_ms,
         feed > 0 ? "key frames forward" : feed < 0 ? "key frames backward" : "normal");
    tp_sample(name, ts, &q0, &t0);
    if (feed) { tf_cur = t0 != 0xffffffffu && t0 ? t0 : pos; tf_jump = 2000; tf_fed = 0; tf_dir = feed; }
    if (start) pl_cmd(start, true);
    next_rep = current_tick; next_s = current_tick + HZ / 4;
    while (TIME_BEFORE(current_tick, end))
    {
        bcm_host_service();
        if (rep && !TIME_BEFORE(current_tick, next_rep)) { pl_cmd(rep, false); next_rep = current_tick + (rep_ms * HZ) / 1000; }
        if (!TIME_BEFORE(current_tick, next_s)) { tp_sample(name, ts, &q, &t); next_s = current_tick + HZ / 4; }
        yield();
    }
    tp_sample(name, ts, &q1, &t1);
    tf_dir = 0;
    tp_r[idx].dseq = (int)(q1 - q0); tp_r[idx].dtime = (int)(t1 - t0);
    BLOG("  probe %s RESULT: %d pictures shown, picture time moved %+d ms in %d ms", name, tp_r[idx].dseq, tp_r[idx].dtime, cfg_probe_ms);
}
static void bcm_trick_probe(void)
{
    char ff[32], rw[32];
    uint32_t base = pl_dur_ms > 400000 ? 120000 : pl_dur_ms / 3, gap = pl_dur_ms > 400000 ? 60000 : pl_dur_ms / 12;
    BLOG("---- trick probe (v46): which FF/REW method moves the picture?  (buttons are ignored for ~%d s)", 6 * (cfg_probe_ms / 1000 + 3));
    snprintf(ff, sizeof ff, "mp_ff %d", cfg_ff_rate); snprintf(rw, sizeof rw, "mp_rewind %d", cfg_ff_rate);
    tp_recover(base);
    tp_test(0, "step-play", NULL, "mp_step 1000", 300, 0, base);         tp_recover(base + gap);
    tp_test(1, "ff", ff, NULL, 0, 0, base + gap);                         tp_recover(base + 2 * gap);
    tp_test(2, "ff+keys", ff, NULL, 0, +1, base + 2 * gap);               tp_recover(base + 3 * gap);
    tp_test(3, "rew", rw, NULL, 0, 0, base + 3 * gap);                    tp_recover(base + 4 * gap);
    tp_test(4, "rew+keys", rw, NULL, 0, -1, base + 4 * gap);              tp_recover(base + 5 * gap);
    tp_test(5, "step-pause", "mp_pause", "mp_step 1000", 300, 0, base + 5 * gap);
    pl_paused = false;
    tp_recover(base);
    /* choose: a method counts when it showed pictures AND the picture time went the right way */
    if (tp_r[2].dseq >= 3 && tp_r[2].dtime >= 3000) sm_ff = SM_FFK;
    else if (tp_r[1].dseq >= 3 && tp_r[1].dtime >= 3000) sm_ff = SM_FF;
    else if (tp_r[0].dseq >= 4 && tp_r[0].dtime >= 1500) sm_ff = SM_STEP;
    else sm_ff = SM_JUMP;
    if (tp_r[4].dseq >= 2 && tp_r[4].dtime <= -2000) sm_rew = SM_FFK;
    else if (tp_r[3].dseq >= 2 && tp_r[3].dtime <= -2000) sm_rew = SM_FF;
    else sm_rew = SM_JUMP;
    BLOG("---- trick probe done: FF uses \"%s\", REW uses \"%s\"", sm_name[sm_ff], sm_name[sm_rew]);
}


/* ---------------------------------------------------------------- v51: audio + A/V sync ----
   Sound: the file's AAC track is played by Rockbox's own playback engine on the ARM (the same decoder as for .m4a music),
   started when the VC's clock starts and kept on the VC clock: every second the VC's elapsed (mp_get_status) and Rockbox's
   elapsed (audio_current_track()->elapsed) are compared; when they differ by more than av_tol ms the side that is ahead
   is paused for the difference (audio_pause/resume, or mp_pause/mp_play).  Pause, FF/REW and taps pause the sound; when
   the picture moves again the sound is moved to the picture time (audio_ff_rewind) and resumed.
   Why not the stock route: the stock firmware sends the AAC stream to the VC too (media_open_streams rt 0x20af74 puts the
   AudioSpecificConfig into the 0x60 stream table, get-frame rt 0x20b39c serves "whichever stream is behind") and mplayer
   plays it with vmcs.bin's audio_play* (48 kHz) -- but how the BCM's audio reaches the WM8758 (which is the I2S clock
   master to the PP5022 in Rockbox) is not known yet, so that route is left for a later experiment (findings 3ar).
   Volume: starts at vol_start % (default 50) of the range up to Rockbox's own volume limit; the click wheel changes it,
   with the stock volume bar (bitmaps 21924/21925/21928/21929, view 22060).  Your previous volume comes back afterwards. */
/* v52: the v51 log showed Rockbox needs ~2.4 s from playlist_start to sound, and that audio_ff_rewind() while the sound
   plays takes effect late (already-decoded PCM keeps playing, the elapsed time jumps back) -- the 1-s corrections then
   piled seeks on top of each other: sound too fast, parts repeated.  New rules:
     * the sound is only ever moved while it is paused, and the picture waits for it:
       PREP(T): sound paused at T (playlist_start + audio_pause at the start, audio_ff_rewind(T) after a seek), the decoder
       fills its PCM buffer meanwhile; when the picture starts moving it is paused (mp_pause) until the sound is ready,
       then both are released together (the one that is behind first, by the measured gap).
     * pause / resume = audio_pause + mp_pause / mp_play + audio_resume, no seek.
     * running: every 2 s a check; only small gaps (av_tol..1 s) are fixed by holding the side that is ahead; a large gap
       re-runs PREP at the picture's time. */
enum { AV_OFF = 0, AV_PREP, AV_RUN };
static int av_state, av_fixes, av_vol_saved, av_checks;
static long av_next, av_prep_t0;
static uint32_t av_target, av_v_last, av_a_last;
static bool av_have, av_started, av_vpaused;      /* av_vpaused: we paused the picture ourselves (PREP) */
static int av_pct_to_vol(int p)
{
    int mn = sound_min(SOUND_VOLUME), lim = global_settings.volume_limit, lo;
    if (lim > sound_max(SOUND_VOLUME) || lim < mn) lim = sound_max(SOUND_VOLUME);
    lo = mn > -60 ? mn : -60;
    if (p <= 0) return mn;
    if (p > 100) p = 100;
    return lo + (lim - lo) * p / 100;
}
static void av_set_vol(int p)
{
    av_vol_pct = p < 0 ? 0 : p > 100 ? 100 : p;
    global_status.volume = av_pct_to_vol(av_vol_pct);
    setvol();
}
static uint32_t av_vc_elapsed(void)
{
    static char r[400];
    bcm_cmd_t0 = current_tick;
    if (bcm_gencmd_watched("mp_get_status", r, sizeof r, 2) < 0) return 0xffffffffu;
    return pl_num(r, "elapsed=");
}
static uint32_t av_rb_elapsed(void)
{   /* v53: + av_offset ms (positive = treat the sound as later, i.e. it is played earlier) */
    struct mp3entry *id3 = audio_current_track();
    return id3 ? (uint32_t)((int32_t)id3->elapsed + cfg_av_offset) : 0xffffffffu;
}
static void av_wait_ms(long ms)
{
    long t = current_tick + (ms * HZ + 999) / 1000;
    while (TIME_BEFORE(current_tick, t)) { bcm_host_service(); yield(); }
}
/* sound to T, paused; the picture is released in av_tick */
static void av_prep(uint32_t t, bool video_paused_by_us)
{
    if (!av_have) return;
    av_target = t; av_vpaused = video_paused_by_us;
    if (!av_started)
    {
        playlist_start(0, t, 0);
        audio_pause();
        av_started = true;
    }
    else
    {
        if (!(audio_status() & AUDIO_STATUS_PAUSE)) audio_pause();
        audio_ff_rewind((long)t);
    }
    av_state = AV_PREP; av_prep_t0 = current_tick; av_v_last = 0xffffffffu; av_next = current_tick;
    BLOG("  audio: sound paused at %u ms, waiting for the picture%s", (unsigned)t, video_paused_by_us ? " (picture held)" : "");
}
/* v53: route 2 (stock): the VideoCore decodes and plays the AAC itself; Rockbox's playback is stopped and only the codec is
   set up -- volume (same 0..100 % scale and limit), sample rate (mplayer opens its output at 48 kHz) and, as an experiment for
   the unknown wiring, the WM8758's AUX inputs mixed into the output (cfg av_mix). */
static bool av_vc_route;
static unsigned int av_hz_saved;
static void av_codec_vc(bool on)
{
    if (on)
    {
        av_hz_saved = mixer_get_frequency();
        if (cfg_av_hz > 0)
        {   /* v54: v53 asked for 48 kHz, but the mixer's *pending* rate was already 48 kHz (Rockbox's last track) while the
               codec still ran at 44.1 kHz, so mixer_set_frequency() returned without touching the codec: the VC's 48 kHz
               sound came out at 44.1 kHz (-8 %, deep) and its clock -- and the picture -- ran 8 % slow.  Switching to
               another rate first forces pcm_apply_settings() to program the codec. */
            mixer_set_frequency(cfg_av_hz == 44100 ? 48000 : 44100);
            mixer_set_frequency((unsigned)cfg_av_hz);
        }
        if (cfg_av_mix & 1)
        {   /* R50/R51: DAC + AUX (AUXxMIXVOL 5 = 0 dB) */
            wmcodec_write(LOUTMIX, LOUTMIX_DACL2LMIX | (1 << 5) | (5 << 6));
            wmcodec_write(ROUTMIX, ROUTMIX_DACR2RMIX | (1 << 5) | (5 << 6));
        }
        BLOG("  audio: VideoCore route -- codec at %u Hz (was %u), AUX mix %s", mixer_get_frequency(), av_hz_saved,
             (cfg_av_mix & 1) ? "on" : "off");
    }
    else
    {
        if (cfg_av_mix & 1) { wmcodec_write(LOUTMIX, LOUTMIX_DACL2LMIX); wmcodec_write(ROUTMIX, ROUTMIX_DACR2RMIX); }
        if (av_hz_saved) mixer_set_frequency(av_hz_saved);
        av_hz_saved = 0;
    }
}
static void av_open(const char *path)
{
    char dir[MAX_PATH], *sl;
    av_state = AV_OFF; av_have = false; av_started = false; av_fixes = 0; av_checks = 0;
    if (!cfg_sound || !path) return;
    av_vol_saved = global_status.volume;
    av_set_vol(cfg_vol_start);
    if (v_sound == 2)
    {
        audio_stop();
        av_vc_route = true;
        av_codec_vc(true);
        BLOG("  audio: the VideoCore plays the AAC track (stock route); volume %d %% = %d dB (limit %d dB)", av_vol_pct,
             global_status.volume, global_settings.volume_limit);
        return;
    }
    snprintf(dir, sizeof dir, "%s", path);
    sl = strrchr(dir, '/'); if (sl) *sl = 0;
    audio_stop();
    if (playlist_create(dir, NULL) < 0 || playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true) < 0)
    { BLOG("  audio: playlist for %s failed -- no sound", path); return; }
    av_have = true;
    BLOG("  audio: Rockbox decodes the AAC track; volume %d %% = %d dB (limit %d dB); mixer %u Hz", av_vol_pct, global_status.volume,
         global_settings.volume_limit, mixer_get_frequency());
    av_prep(0, false);
}
static void av_close(void)
{
    if (av_vc_route)
    {
        av_codec_vc(false); av_vc_route = false;
        if (!av_have) { global_status.volume = av_vol_saved; setvol(); BLOG("  audio: volume back to %d dB", av_vol_saved); }
    }
    if (!av_have) return;
    audio_stop();
    global_status.volume = av_vol_saved; setvol();
    BLOG("  audio: stopped after %d sync corrections; volume back to %d dB", av_fixes, av_vol_saved);
    av_have = false; av_state = AV_OFF;
}
/* picture paused / scrub start: hold the sound */
static void av_hold(void)
{
    if (!av_have || !av_started) return;
    if (!(audio_status() & AUDIO_STATUS_PAUSE)) audio_pause();
}
/* PLAY after a pause (no seek): the sound continues where it was */
static void av_resume(void)
{
    if (!av_have || !av_started || av_state != AV_RUN) return;
    audio_resume();
    av_next = current_tick + 2 * HZ; av_a_last = 0xffffffffu;
}
static void av_tick(void)
{
    uint32_t v, a; int d;
    if (!av_have || pl_paused || sc_active || TIME_BEFORE(current_tick, av_next)) return;
    if (av_state == AV_PREP)
    {
        bool ready;
        av_next = current_tick + HZ / 20;
        if (!av_vpaused)
        {   /* wait for the picture to move (two rising readings), then hold it */
            v = av_vc_elapsed();
            if (v == 0xffffffffu) return;
            if (av_v_last == 0xffffffffu || v <= av_v_last) { av_v_last = v; return; }
            pl_cmd("mp_pause", false);
            av_vpaused = true;
        }
        /* sound ready: the decoder has filled some PCM buffer while paused (or 4 s passed) */
        ready = (current_tick - av_prep_t0 > HZ * 4 / 10 && pcmbuf_used_descs() >= cfg_av_ready)   /* >= 0.4 s: a seek has */
                || current_tick - av_prep_t0 > 4 * HZ;                                          /* flushed the old PCM  */
        if (!ready) return;
        v = av_vc_elapsed(); a = av_rb_elapsed();
        if (v == 0xffffffffu) v = av_target;
        if (a == 0xffffffffu) a = av_target;
        d = (int)(v - a);                              /* > 0: picture ahead */
        if (d > 1000 || d < -1000) d = 0;
        if (d > 0) { audio_resume(); av_wait_ms(d); pl_cmd("mp_play", false); }
        else       { pl_cmd("mp_play", false); av_wait_ms(-d); audio_resume(); }
        BLOG("  audio: released together at picture %u / sound %u ms (gap %+d ms, pcm descs %d, %ld ms after prep)",
             (unsigned)v, (unsigned)a, d, pcmbuf_used_descs(), (current_tick - av_prep_t0) * 1000 / HZ);
        {   static bool id3_done;                      /* v53: what Rockbox thinks it is playing (the "too fast" report) */
            struct mp3entry *id3 = audio_current_track();
            if (id3 && !id3_done)
            {
                id3_done = true;
                BLOG("  audio: Rockbox track: codec %d, %lu Hz, %lu kbit/s, length %lu ms, upsampling corr %d; mixer %u Hz",
                     (int)id3->codectype, (unsigned long)id3->frequency, (unsigned long)id3->bitrate, (unsigned long)id3->length,
                     (int)id3->needs_upsampling_correction, mixer_get_frequency());
            }
        }
        av_vpaused = false;
        av_state = AV_RUN; av_next = current_tick + 2 * HZ; av_a_last = 0xffffffffu;
        return;
    }
    /* AV_RUN: check every 2 s; correct only when the sound is really running (advanced 1.5..2.5 s since the last check) */
    av_next = current_tick + 2 * HZ;
    if (audio_status() & AUDIO_STATUS_PAUSE) return;
    v = av_vc_elapsed(); a = av_rb_elapsed();
    if (v == 0xffffffffu || a == 0xffffffffu) return;
    if (av_a_last == 0xffffffffu || a < av_a_last + 1500 || a > av_a_last + 2500) { av_a_last = a; return; }
    av_a_last = a;
    d = (int)(a - v);                                  /* > 0: sound ahead */
    if (++av_checks <= 5 || av_checks % 15 == 0)
        BLOG("  audio check: picture %u ms, sound %u ms (%+d ms)", (unsigned)v, (unsigned)a, d);
    if (d <= cfg_av_tol && d >= -cfg_av_tol) return;
    av_fixes++;
    if (d > 1000 || d < -1000)
    {
        BLOG("  audio sync #%d: %+d ms -> re-prepare the sound at the picture's time", av_fixes, d);
        pl_cmd("mp_pause", false);
        av_prep(v, true);
        return;
    }
    if (d > 0) { audio_pause(); av_wait_ms(d); audio_resume(); }
    else       { pl_cmd("mp_pause", false); av_wait_ms(-d); pl_cmd("mp_play", false); }
    av_a_last = 0xffffffffu;
    if (av_fixes <= 20 || av_fixes % 20 == 0)
        BLOG("  audio sync #%d: picture %u ms, sound %u ms (%+d ms) -> held the %s", av_fixes, (unsigned)v, (unsigned)a, d, d > 0 ? "sound" : "picture");
}

/* ---- scrubbing */
static uint32_t sc_speed_jump(long held)
{   /* ms of video per second of holding: 4x, 10x, 30x, 90x */
    return held < 2 * HZ ? 4000 : held < 5 * HZ ? 10000 : held < 9 * HZ ? 30000 : 90000;
}
static void scrub_begin(int dir)
{
    char c[48];
    pl_status();
    sc_active = true; sc_dir = dir; sc_ticks = 0; sc_t0 = current_tick; sc_next = current_tick;
    sc_pos = pl_el_ms; pl_target_ms = sc_pos; sc_jump = 4000;
    sc_was_playing = !pl_paused;
    sc_method = cfg_scrub_mode != SM_AUTO ? cfg_scrub_mode : (dir > 0 ? sm_ff : sm_rew);
    if (sc_method == SM_STEP && dir < 0) sc_method = SM_JUMP;
    BLOG("  scrub: %s held at %u ms, method %s", dir > 0 ? "FF" : "REW", (unsigned)sc_pos, sm_name[sc_method]);
    av_hold();
    if (sc_method == SM_FF || sc_method == SM_FFK)
    {
        if (sc_method == SM_FFK)
        {   uint32_t dt = 0, kp; pl_disp(&dt, NULL);
            tf_cur = dt && dt != 0xffffffffu ? dt : sc_pos; tf_jump = 1000; tf_fed = 0; tf_dir = dir;
            m4_key_at(tf_cur, &kp);                    /* v78: FF starts at the next key frame, not the one before (a jump */
            if (dir > 0 && tf_cur > kp + 1000)         /* back first: the v76 log went 5.4 s -> 2.3 s -> 12.3 s)          */
                tf_fed = 1;                            /* (tf_next_key then steps forward from here at once) */
        }
        sc_rate = cfg_ff_rate;
        snprintf(c, sizeof c, "%s %d", dir > 0 ? "mp_ff" : "mp_rewind", sc_rate);
        pl_cmd(c, true);
    }
    else if (sc_method == SM_SEEKSTEP && sc_was_playing) { pl_cmd("mp_pause", true); pl_paused = true; }
    else if (sc_method == SM_STEP && !sc_was_playing) { pl_cmd("mp_play", true); pl_paused = false; }
    osd_show_for(0);
}
static void scrub_tick(void)
{
    char c[48];
    long held;
    uint32_t k, dt;
    if (!sc_active) return;
    if (sc_btn && !(button_status() & sc_btn)) sc_btn = 0;
    if (TIME_BEFORE(current_tick, sc_next)) return;
    held = current_tick - sc_t0;
    switch (sc_method)
    {
    case SM_FF: case SM_FFK:
        sc_next = current_tick + (cfg_scrub_tick * HZ) / 1000;
        tf_jump = sc_speed_jump(held) / 8;                  /* key frames fed per second of video shown: ~8 */
        if (cfg_ff_accel)
        {   /* v47: faster the longer the button is held: rate x1 (~9x, v46 probe), x2 after 3 s, x4 after 7 s */
            int want = cfg_ff_rate * (held < 3 * HZ ? 1 : held < 7 * HZ ? 2 : 4);
            if (want != sc_rate)
            {
                sc_rate = want;
                snprintf(c, sizeof c, "%s %d", sc_dir > 0 ? "mp_ff" : "mp_rewind", sc_rate);
                pl_cmd(c, true);
            }
        }
        if (pl_disp(&dt, NULL) == 0 && dt != 0xffffffffu) { sc_pos = dt; pl_target_ms = dt; }
        break;
    case SM_STEP:
        sc_next = current_tick + (cfg_scrub_tick * HZ) / 1000;
        snprintf(c, sizeof c, "mp_step %u", (unsigned)(sc_speed_jump(held) * (unsigned)cfg_scrub_tick / 1000));
        pl_cmd(c, sc_ticks < 3);
        if (pl_disp(&dt, NULL) == 0 && dt != 0xffffffffu) { sc_pos = dt; pl_target_ms = dt; }
        break;
    case SM_SEEKSTEP: case SM_JUMP: default:
        sc_next = current_tick + ((sc_method == SM_JUMP ? cfg_jump_tick : cfg_scrub_tick) * HZ) / 1000;
        sc_jump = sc_speed_jump(held) * (unsigned)(sc_method == SM_JUMP ? cfg_jump_tick : cfg_scrub_tick) / 1000;
        k = pl_key_step(sc_pos, sc_dir, sc_jump);
        if (k == sc_pos && sc_ticks) break;
        sc_pos = k; pl_target_ms = k;
        snprintf(c, sizeof c, "mp_seek %u", (unsigned)k);
        pl_cmd(c, sc_ticks < 3);
        if (sc_method == SM_SEEKSTEP && cfg_step_ms > 0) { snprintf(c, sizeof c, "mp_step %d", cfg_step_ms); pl_cmd(c, sc_ticks < 3); }
        break;
    }
    if (sc_ticks < 5 || sc_ticks % 10 == 0)
        BLOG("  scrub tick %d (%s): at %u ms, held %ld.%ld s", sc_ticks, sm_name[sc_method], (unsigned)sc_pos, held / HZ, held % HZ / 10);
    sc_ticks++;
    osd_present();
}
static void scrub_end(void)
{
    char c[40]; uint32_t k, dt = 0;
    sc_active = false;
    if (sc_method == SM_FF || sc_method == SM_FFK)
    {
        int n0 = v_seek_n;
        long t;
        tf_dir = 0;
        if (pl_disp(&dt, NULL) == 0 && dt != 0xffffffffu && dt) sc_pos = dt;
        pl_cmd("mp_play", true); pl_paused = false;
        /* v78: mp_play makes the VC seek by itself (to the key frame it reached); v47-v77 then seeked again to the frame on
           screen -- a second flush + refill (~1 s more before the picture moved; v76 log: 4.8 s from release to motion).
           Now the VC's own position is kept when it has one. */
        t = current_tick + HZ * 4 / 10;
        while (v_seek_n == n0 && TIME_BEFORE(current_tick, t)) { bcm_host_service(); yield(); }
        if (v_seek_n != n0 && cfg_ff_resume == 1)
        {
            BLOG("  scrub: the VideoCore resumed at %u ms itself (picture was at %u ms): no second seek",
                 (unsigned)v_seek_kms, (unsigned)sc_pos);
            sc_pos = v_seek_kms;
        }
        else
        {
            m4_key_at(sc_pos, &k);
            snprintf(c, sizeof c, "mp_seek %u", (unsigned)k); pl_cmd(c, true);
            sc_pos = k;
        }
        if (!sc_was_playing) { pl_cmd("mp_pause", true); pl_paused = true; }
    }
    else if (sc_method == SM_SEEKSTEP && sc_was_playing) { pl_cmd("mp_play", true); pl_paused = false; }
    BLOG("  scrub: released at %u ms after %d ticks (%s)", (unsigned)sc_pos, sc_ticks, sm_name[sc_method]);
    pl_el_ms = sc_pos; vr_el_tick = current_tick; st_seeked();   /* v73 */
    av_prep(sc_pos, false);                            /* v52: sound moved (paused) to the new place, released with the picture */
    mon_start("scrub released");
    osd_show_for(pl_paused ? 0 : 3000);
}
static void pl_tap(int dir)
{
    char c[48]; uint32_t k;
    if (cfg_tap_secs <= 0) return;
    pl_status();
    k = pl_key_step(pl_el_ms, dir, (uint32_t)cfg_tap_secs * 1000u);
    av_hold();
    snprintf(c, sizeof c, "mp_seek %u", (unsigned)k);
    pl_cmd(c, true);
    pl_el_ms = k; pl_target_ms = k; vr_el_tick = current_tick; st_seeked();   /* v73 */
    av_prep(k, false);
    mon_start(dir > 0 ? "tap >>|" : "tap |<<");
    osd_show_for(pl_paused ? 0 : 3000);
}
static void bcm_osd_tick(void)
{
    st_tick();                                         /* v73: subtitles */
    if (sc_btn && !sc_active && current_tick - sc_press >= (HZ * 4) / 10 && (button_status() & sc_btn))
        scrub_begin(sc_btn & (BUTTON_RIGHT | BUTTON_RC_RIGHT) ? 1 : -1);
    if (sc_active && !sc_btn && !sc_remote) scrub_end();
    scrub_tick();
    mon_tick();
    av_tick();
    if (av_vol_dirty && cfg_osd)
    {   /* v53: volume bar.  v54: once the bar is on screen, every click is drawn at once straight into the visible image */
        bool direct = osd_ok > 0 && osd_s[2].win[0] && (!bcm_tv || !cfg_tv_osd || osd_s[2].tvwin);
        (void)cfg_vol_direct;
        {   /* v56: every click: only the bar pixels that changed (a 3 % step = ~7 columns x 24 rows) */
            unsigned long t0 = USEC_TIMER;
            av_vol_dirty = false; av_vol_drawn = current_tick;
            if (osd_ok < 0) osd_init();
            if (!osd_visible) osd_vol_only = true;
            if (bcm_tv && (!osd_tv_until || TIME_BEFORE(osd_tv_until, av_vol_until))) osd_tv_until = av_vol_until;
            osd_present_mask(4);
            if (direct) { vol_direct_n++; vol_us_direct += USEC_TIMER - t0; }
            else { vol_full_n++; vol_us_full += USEC_TIMER - t0; }
            if (!bcm_tv && !pl_paused && !sc_active && (!osd_hide_at || TIME_BEFORE(osd_hide_at, av_vol_until))) osd_hide_at = av_vol_until;
            osd_next = current_tick + HZ;
        }
    }
    if (vol_direct_n + vol_full_n && av_vol_until && !TIME_BEFORE(current_tick, av_vol_until))
    {   /* v54: one line per volume burst */
        BLOG("  volume %d %%: %d direct bar updates (avg %lu us), %d full (avg %lu us)", av_vol_pct, vol_direct_n,
             vol_direct_n ? vol_us_direct / vol_direct_n : 0, vol_full_n, vol_full_n ? vol_us_full / vol_full_n : 0);
        vol_direct_n = vol_full_n = 0; vol_us_direct = vol_us_full = 0;
    }
    if (bcm_tv && osd_visible && (osd_s[0].tvwin || osd_s[2].tvwin) && !osd_tv_want(0))
        osd_present_mask(0);                           /* v56: removes the TV windows (the iPod screen keeps its overlay) */
    if (!osd_visible || sc_active) return;
    if (osd_hide_at && !TIME_BEFORE(current_tick, osd_hide_at) && !pl_paused) { osd_hide(); return; }
    if (!TIME_BEFORE(current_tick, osd_next))
    {
        osd_next = current_tick + HZ;
        if (av_vol_until && TIME_BEFORE(current_tick, av_vol_until)) return;   /* v54: the volume bar is drawn directly */
        pl_status();
        if (osd_vol_only) osd_present_mask(4); else osd_present();
    }
}
/* ---- v63: the dock / remotes control the video (iAP: apps/iap/)
   Accessories reach the player two ways:
   * button-style (Simple Remote ContextButtonStatus: an Apple Universal Dock's IR remote, inline remotes, most AV docks):
     they arrive as BUTTON_RC_* in the button queue with press / repeat / release, and are treated exactly like the
     iPod's own buttons (>>| tap = jump, hold = FF; |<< likewise; play/pause; MENU = stop; centre = controls;
     volume +/- and up/down = volume);
   * commands (VideoButtonStatus, Extended Interface PlayControl, Display Remote volume, the ContextButtonStatus bits
     that have no button: play, pause, stop, mute, chapters, power off): queued here by the iAP thread and run by the
     player loop.  "Next / previous video" end this video and the file browser starts the next / previous video file of
     the folder (previous restarts the video when it is more than 3 s in, like Apple's firmware). */
enum { VR_TOGGLE = 1, VR_PLAY, VR_PAUSE, VR_STOP, VR_NEXT, VR_PREV, VR_FF, VR_REW, VR_FFREW_END,
       VR_VOL_UP, VR_VOL_DOWN, VR_VOL_SET, VR_MUTE, VR_CH_NEXT, VR_CH_PREV, VR_OSD };
static volatile bool bcm_video_running;
static volatile unsigned char vr_q[16];
static volatile short vr_qarg[16];
static volatile unsigned vr_w, vr_r;
static volatile long vr_status_asked;
static void bcm_out_switch(void);         /* v78 */
int bcm_video_next_request;              /* read by apps/filetree.c: +1 / -1 = play the next / previous video */
/* v74: TV series (apps/filetree.c): this video is an episode with autoplay on; "stop after this episode" (hold PLAY);
   the last video played to its end (not stopped) */
bool bcm_tvs_episode, bcm_tvs_stop_after, bcm_video_finished;
bool bcm_video_active(void) { return bcm_video_running; }
int bcm_video_remote(int cmd, int arg)
{
    if (!bcm_video_running) return 0;
    if (vr_w - vr_r < 16) { vr_q[vr_w & 15] = (unsigned char)cmd; vr_qarg[vr_w & 15] = (short)arg; vr_w++; }
    return 1;
}
/* state: 0 stopped, 1 playing, 2 paused, 3 FF, 4 REW (iAP numbering) */
bool bcm_video_status(uint32_t *len_ms, uint32_t *pos_ms, int *state)
{
    uint32_t pos;
    if (!bcm_video_running) return false;
    vr_status_asked = current_tick;
    pos = sc_active ? sc_pos : pl_el_ms;
    if (!pl_paused && !sc_active && vr_el_tick) pos += (uint32_t)(current_tick - vr_el_tick) * (1000 / HZ);
    if (pl_dur_ms && pos > pl_dur_ms) pos = pl_dur_ms;
    if (len_ms) *len_ms = pl_dur_ms;
    if (pos_ms) *pos_ms = pos;
    if (state) *state = sc_active ? (sc_dir > 0 ? 3 : 4) : pl_paused ? 2 : 1;
    return true;
}
static void pl_set_paused(bool pause, char *resp, size_t rn)
{
    const char *cmd = pause ? "mp_pause" : "mp_play";
    int r2;
    if (pause == pl_paused) return;
    if (pause) av_hold();                              /* pausing: hold the sound now */
    else pl_status();                                  /* v63: fresh position for the dock's status */
    bcm_cmd_t0 = current_tick; r2 = bcm_gencmd_watched(cmd, resp, rn, 3); bcm_log_text(cmd, r2, resp);
    pl_paused = pause;
    if (!pl_paused) { av_resume(); mon_start("PLAY -> mp_play"); }
    else pl_status();
    osd_show_for(pl_paused ? 0 : 3000);
}
static void pl_restart(void)
{
    char c[24];
    av_hold();
    snprintf(c, sizeof c, "mp_seek %u", 0u);
    pl_cmd(c, true);
    pl_el_ms = 0; pl_target_ms = 0; vr_el_tick = current_tick; st_seeked();   /* v73 */
    av_prep(0, false);
    mon_start("dock: restart");
    osd_show_for(pl_paused ? 0 : 3000);
}
static void pl_vol_show(void)
{
    av_vol_until = current_tick + 2 * HZ;
    av_vol_dirty = true;
}
static const char * const vr_name[] = { "?", "play/pause", "play", "pause", "stop", "next video", "previous video",
    "FF", "REW", "end FF/REW", "volume +", "volume -", "volume", "mute", "next chapter", "previous chapter", "controls" };
/* returns 1 to end the video */
static int vr_drain(char *resp, size_t rn)
{
    static int mute_pct = -1;
    while (vr_r != vr_w)
    {
        int c = vr_q[vr_r & 15], a = vr_qarg[vr_r & 15];
        vr_r++;
        if (c == VR_VOL_SET) BLOG("  dock: volume set to %d %%", a);
        else BLOG("  dock: %s", c < (int)(sizeof vr_name / sizeof vr_name[0]) ? vr_name[c] : "?");
        av_boost_until = current_tick + 3 * HZ;
        if (!av_boosted && cfg_boost) av_boost(true);
        switch (c)
        {
        case VR_TOGGLE: pl_set_paused(!pl_paused, resp, rn); break;
        case VR_PLAY:   pl_set_paused(false, resp, rn); break;
        case VR_PAUSE:  pl_set_paused(true, resp, rn); break;
        case VR_STOP:   return 1;
        case VR_NEXT:   bcm_video_next_request = 1; return 1;
        case VR_PREV:
            pl_status();
            if (pl_el_ms > 3000) pl_restart();
            else { bcm_video_next_request = -1; return 1; }
            break;
        case VR_FF: case VR_REW:
            if (sc_active) scrub_end();
            sc_btn = 0; sc_remote = true;
            scrub_begin(c == VR_FF ? 1 : -1);
            break;
        case VR_FFREW_END: sc_remote = false; break;   /* bcm_osd_tick ends the scrub */
        case VR_VOL_UP:   av_set_vol(av_vol_pct + cfg_vol_step); mute_pct = -1; pl_vol_show(); break;
        case VR_VOL_DOWN: av_set_vol(av_vol_pct - cfg_vol_step); mute_pct = -1; pl_vol_show(); break;
        case VR_VOL_SET:  av_set_vol(a); mute_pct = -1; pl_vol_show(); break;
        case VR_MUTE:
            if (mute_pct < 0) { mute_pct = av_vol_pct; av_set_vol(0); }
            else { av_set_vol(mute_pct); mute_pct = -1; }
            pl_vol_show();
            break;
        case VR_CH_NEXT: pl_tap(1); break;
        case VR_CH_PREV: pl_tap(-1); break;
        case VR_OSD: if (osd_visible && !pl_paused) osd_hide(); else osd_show_for(3000); break;
        }
    }
    return 0;
}
/* the dock's remote buttons (BUTTON_RC_*) behave like the iPod's own */
static int rc_to_local(int base)
{
    switch (base)
    {
    case BUTTON_RC_RIGHT: return BUTTON_RIGHT;
    case BUTTON_RC_LEFT: return BUTTON_LEFT;
    case BUTTON_RC_PLAY: return BUTTON_PLAY;
    case BUTTON_RC_MENU: return BUTTON_MENU;
    case BUTTON_RC_SELECT: return BUTTON_SELECT;
    case BUTTON_RC_VOL_UP: case BUTTON_RC_UP: return BUTTON_SCROLL_FWD;
    case BUTTON_RC_VOL_DOWN: case BUTTON_RC_DOWN: return BUTTON_SCROLL_BACK;
    default: return base;
    }
}

/* returns 1 when the user wants to stop.  The wheel is the volume (v51), never seeking. */
static int bcm_player_input(char *resp, size_t rn)
{
    int b, base, raw;
    if (vr_r != vr_w && vr_drain(resp, rn)) return 1;  /* v63 */
    b = button_get_w_tmo(0);
    if (b == BUTTON_NONE) return 0;
    if (b == SYS_PHONE_UNPLUGGED || b == SYS_PHONE_PLUGGED)
    {   /* v76: headphones out = pause (Settings > Playback > Pause on Headphone Unplug); back in = resume with
           "Pause and Resume".  Not on a TV or a dock: the sound is not in the headphones there. */
        static bool hp_paused;
        BLOG("  headphones %s", b == SYS_PHONE_PLUGGED ? "plugged in" : "pulled out");
        if (global_settings.unplug_mode && !bcm_tv && !iap_dock_name())
        {
            if (b == SYS_PHONE_UNPLUGGED && !pl_paused) { pl_set_paused(true, resp, rn); hp_paused = true; osd_show_for(3000); }
            else if (b == SYS_PHONE_PLUGGED && hp_paused && pl_paused && global_settings.unplug_mode > 1)
                pl_set_paused(false, resp, rn);
        }
        if (b == SYS_PHONE_PLUGGED) hp_paused = false;
        return 0;
    }
    if (IS_SYSEVENT(b)) return 0;
    av_boost_until = current_tick + 3 * HZ;            /* v57 */
    if (!av_boosted && cfg_boost) av_boost(true);
    raw = b & ~(BUTTON_REPEAT | BUTTON_REL);
    base = rc_to_local(raw);                           /* v63: a remote button = the same iPod button */
    if (raw != base)
    {
        b = (b & (BUTTON_REPEAT | BUTTON_REL)) | base;
        if (raw == BUTTON_RC_RIGHT || raw == BUTTON_RC_LEFT) base = raw;   /* hold detection watches the remote bit */
    }
    if (base == BUTTON_RIGHT || base == BUTTON_LEFT || base == BUTTON_RC_RIGHT || base == BUTTON_RC_LEFT)
    {
        int dir = base & (BUTTON_RIGHT | BUTTON_RC_RIGHT) ? 1 : -1;
        if (b & BUTTON_REL)
        {
            if (sc_btn == base)
            {
                sc_btn = 0;
                if (!sc_active) pl_tap(dir);           /* short press */
            }
            return 0;
        }
        if (!(b & BUTTON_REPEAT) && !sc_btn) { sc_btn = base; sc_press = current_tick; }
        else if ((b & BUTTON_REPEAT) && sc_btn == base && !sc_active && current_tick - sc_press >= (HZ * 4) / 10)
            scrub_begin(dir);
        return 0;
    }
    if (base == BUTTON_SCROLL_FWD || base == BUTTON_SCROLL_BACK)
    {   /* v51: the wheel is the volume, like stock.  v53: applied at once (the codec volume is one register write);
           fast turning takes bigger steps; the stock volume bar is redrawn at most 10x/s from bcm_osd_tick, only the
           bottom strip (v51 redrew all three strips + read the status on every click: ~0.2 s per click) */
        static long last_scroll;
        int step = current_tick - last_scroll < HZ / 12 ? cfg_vol_step * 2 : cfg_vol_step;
        if (b & BUTTON_REL) return 0;
        last_scroll = current_tick;
        av_set_vol(av_vol_pct + (base == BUTTON_SCROLL_FWD ? step : -step));
        av_vol_until = current_tick + 2 * HZ;
        av_vol_dirty = true;
        return 0;
    }
    if (base == BUTTON_SELECT && st_n > 0)
    {   /* v73: with subtitles available: a short press = the controls (on release), held 0.6 s = next subtitle track / off */
        if (!(b & (BUTTON_REL | BUTTON_REPEAT))) { st_sel_down = true; st_sel_done = false; st_sel_t0 = current_tick; return 0; }
        if (!st_sel_down) return 0;
        if (b & BUTTON_REL)
        {
            st_sel_down = false;
            if (!st_sel_done) { if (osd_visible && !pl_paused) osd_hide(); else osd_show_for(3000); }
        }
        else if (!st_sel_done && current_tick - st_sel_t0 >= HZ * 6 / 10) { st_sel_done = true; st_cycle(); }
        return 0;
    }
    if (base == BUTTON_PLAY && bcm_tvs_episode)
    {   /* v74: a TV episode with autoplay: a short press = pause (on release), held 0.8 s = stop after this episode on/off */
        static bool down, done;
        static long t0;
        if (!(b & (BUTTON_REL | BUTTON_REPEAT))) { down = true; done = false; t0 = current_tick; return 0; }
        if (!down) return 0;
        if (b & BUTTON_REL) { down = false; if (!done) pl_set_paused(!pl_paused, resp, rn); }
        else if (!done && current_tick - t0 >= HZ * 8 / 10)
        {
            done = true;
            bcm_tvs_stop_after = !bcm_tvs_stop_after;
            st_toast(str(bcm_tvs_stop_after ? LANG_TVS_STOP_AFTER : LANG_TVS_PLAY_NEXT));
            BLOG("  TV show: %s", bcm_tvs_stop_after ? "stop after this episode" : "the next episode follows");
        }
        return 0;
    }
    if (base == BUTTON_MENU)
    {   /* v78: a short press = stop (on release, as before but on letting go), held 1 s = the picture to the other screen
           (TV <-> iPod).  The way out when Video Out is On with no TV: the iPod's own screen is not driven then. */
        static bool down, done;
        static long t0;
        if (!(b & (BUTTON_REL | BUTTON_REPEAT))) { down = true; done = false; t0 = current_tick; return 0; }
        if (!down) return 0;                             /* a release without its press (pressed before the video) */
        if (b & BUTTON_REL) { down = false; return done ? 0 : 1; }
        if (!done && current_tick - t0 >= HZ) { done = true; bcm_out_switch(); }
        return 0;
    }
    if (b & (BUTTON_REL | BUTTON_REPEAT)) return 0;
    if (b == BUTTON_MENU) return 1;
    if (b == BUTTON_PLAY)
        pl_set_paused(!pl_paused, resp, rn);
    else if (b == BUTTON_SELECT)
    {
        if (osd_visible && !pl_paused) osd_hide(); else osd_show_for(3000);
    }
    return 0;
}

/* v48.1: going back to the browser.  The panel keeps whatever the VC sent last while the BCM is rebooted into Rockbox's display
   firmware (~0.5 s): with v48 that was a half-overwritten video frame.  So first put the browser itself on screen: Rockbox's
   frame buffer (untouched while the video played) goes into a VC image on the top layer. */
static void bcm_player_cover(void)
{
    uint32_t img = 0, addr = 0, o[4], pl[8];
    int y;
    long t;
    if (osd_alloc(1, LCD_WIDTH, LCD_HEIGHT, &img, &addr) != 0) { img = gfx_img[0]; addr = gfx_addr[0]; }
    if (!img) return;
    for (y = 0; y < LCD_HEIGHT; y += 16)
    {
        bcm_io_lock(); bcm_io_write(addr + (unsigned)y * LCD_WIDTH * 2, FBADDR(0, y), 16 * LCD_WIDTH * 2); bcm_io_unlock();
    }
    memset(pl, 0, sizeof pl);
    pl[1] = (uint32_t)(cfg_osd_layer + 20); pl[3] = img;
    ((uint16_t *)pl)[12] = LCD_WIDTH; ((uint16_t *)pl)[13] = LCD_HEIGHT;
    bcm_gfx_rpc(1, NULL, 0, o, NULL, 300);
    bcm_gfx_rpc(3, pl, 0x20, o, NULL, 300);
    bcm_gfx_rpc(2, NULL, 0, o, NULL, 300);
    for (t = current_tick + HZ / 10; TIME_BEFORE(current_tick, t); ) { bcm_host_service(); yield(); }   /* ~3 panel refreshes */
    BLOG("  player: browser picture put on top (image %08x, window %08x)", (unsigned)img, (unsigned)o[0]);
}

/* v57: CPU boost only while the ARM has real work: the first 10 s (start-up, pre-buffer), while scrubbing and for 3 s after any
   button.  With the stock sound route the VideoCore decodes picture AND sound; the ARM only reads ~60 KB/s from the disk and
   copies it over the bus, which the unboosted clock does easily (cfg boost: 0 never, 1 always, 2 auto). */
static void av_boost(bool on)
{
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (on == av_boosted || (on && cfg_boost == 0)) return;
    cpu_boost(on); av_boosted = on;
#else
    (void)on;
#endif
}
static void av_boost_tick(long run_t0)
{
    if (cfg_boost != 2) return;
    if (sc_active || current_tick - run_t0 < 10 * HZ || v_sound != 2 || TIME_BEFORE(current_tick, av_boost_until)) av_boost(true);
    else av_boost(false);
}
static int bcm_hw_model(int log);
static void bcm_video_run(int secs)
{
    long end = current_tick + secs * HZ, next_log = current_tick + 2 * HZ, next_stat = current_tick + 5 * HZ;
    long probe_tick = cfg_probe_at > 0 ? current_tick + cfg_probe_at * HZ : 0;
    long dump1 = cfg_dump && cfg_dump_at1 > 0 ? current_tick + cfg_dump_at1 * HZ : 0;
    long dump2 = cfg_dump && cfg_dump_at2 > 0 ? current_tick + cfg_dump_at2 * HZ : 0;
    static char resp[1400];                           /* v29: the v28 logs cut mp_get_stats at 256 bytes (lost_frame_assert_d...) */
    long pw1 = 0, pw2 = 0;
    static int framedumped;
    BLOG("  >>> LOOK: mp_play sent -- VIDEO should appear from here (variant post=%d)", cfg_post);
    if (cfg_post >= 1 && gfx_front >= 0)
    {
        uint32_t o[4], w4[4] = { gfx_win[gfx_front], 0, 0, 0 };
        if (cfg_post >= 2 && gfx_win[gfx_front])
        {
            bcm_gfx_rpc(1, NULL, 0, o, NULL, 300);
            bcm_gfx_rpc(4, w4, 0x10, o, NULL, 300);
            bcm_gfx_rpc(2, NULL, 0, o, NULL, 300);
            BLOG("  post: our UI window %08x removed", (unsigned)w4[0]);
            gfx_win[gfx_front] = 0;
        }
        gfx_front = -1;                                /* no more status-screen presents */
        BLOG("  post: status-screen updates stopped");
    }
    if (cfg_post == 3) { pw1 = current_tick + HZ; pw2 = current_tick + 4 * HZ; }
    int gf_last = v_getframes, gf_log = v_getframes, nstat = 0;
    long last_req = current_tick, run_t0 = current_tick, bl_next = current_tick;
    if (!bcm_tv)
    {   /* v57: on the TV the iPod's own backlight follows Rockbox's normal timeout (the panel itself stays on) */
        backlight_set_brightness(MAX_BRIGHTNESS_SETTING);  /* v47.1: video at full brightness (restored when it ends) */
        backlight_set_on_button_hold(2);                   /* v50: the hold switch does not turn the screen off while a video plays */
    }
    av_boost(true);                                    /* v51: boosted; v57: only while it is needed (av_boost_tick) */
#ifdef HAVE_BATTERY_SMOOTH_API
    powermgmt_set_load_hint(3, true);                  /* v58: the battery reading knows the video decoder is running */
#endif
    av_open(bcm_play_path ? bcm_play_path : v_path);   /* v51: sound (starts when the picture moves) */
    backlight_on();
    bool hold_was = button_hold();
    bool probed = !cfg_trickprobe;
    bool av_checked = false;
    uint32_t rate_e0 = 0; long rate_t0 = 0; int rate_seeks = 0;   /* v54: VC clock speed vs real time */
    if (v_sound == 2 && cfg_vcvol >= 0)
    {   char c[24]; int r2;
        snprintf(c, sizeof c, "mp_setvol %d", cfg_vcvol);
        bcm_cmd_t0 = current_tick; r2 = bcm_gencmd_watched(c, resp, sizeof resp, 2); bcm_log_text(c, r2, resp);
    }
    mon_start("mp_play (start-up pre-buffer)");
    if (bcm_tv) osd_show_for(3000);                    /* v55: TV: controls + TV symbol on the LCD the whole time; v56: 3 s on the TV */
    vr_r = vr_w; sc_remote = false; vr_el_tick = 0;      /* v63: dock commands from before this video are dropped */
    st_seeked(); st_fast_until = current_tick + 10 * HZ;  /* v73: subtitles: frequent position reads while the picture starts */
    bcm_video_running = true;
    while (TIME_BEFORE(current_tick, end) && !v_stop)
    {
        bcm_host_service();
        if (vr_status_asked && current_tick - vr_status_asked < 5 * HZ && !sc_active
            && (!vr_el_tick || current_tick - vr_el_tick > 2 * HZ))
            pl_status();                               /* v63: a dock shows the position: keep it fresh */
        if (bcm_check_dead()) break;
        if (v_eos_tick && current_tick - v_eos_tick > 4 * HZ) { bcm_video_finished = true; break; }   /* v74: it played to the end */
        if (button_hold() != hold_was)
        {   /* v50: show the lock (or its removal) for 3 s */
            hold_was = !hold_was;
            BLOG("  hold switch %s", hold_was ? "ON" : "off");
            osd_show_for(3000);
        }
        if (!pl_paused && !TIME_BEFORE(current_tick, bl_next))
        {   /* v47.1: backlight always on while the video plays (paused: Rockbox's own timeout and idle power-off apply).
               v57: TV out: only the power-off timer is held off -- the backlight may time out */
            bl_next = current_tick + HZ / 2;
            if (!bcm_tv) backlight_on();
            reset_poweroff_timer();
        }
        av_boost_tick(run_t0);
        ra_tick();                                     /* v59 */
        if (!probed && current_tick - run_t0 > 5 * HZ)
        {   /* v46: once per run, ~5 s in: measure the FF/REW candidates (buttons are not read meanwhile) */
            probed = true; mon_until = 0;
            pl_status();
            if (pl_dur_ms > 60000) bcm_trick_probe(); else BLOG("trick probe skipped: video shorter than 60 s");
            while (button_get_w_tmo(0) != BUTTON_NONE) ;         /* drop presses made during the probe */
        }
        else if (bcm_player_input(resp, sizeof resp))
        {
            BLOG("  user: MENU -> stop");
            if (bcm_player_mode) bcm_player_cover();
            break;
        }
        bcm_osd_tick();
        if (st_cur >= 0 && st_ok == 0 && current_tick - run_t0 > HZ && !sc_active)
            st_setup();                                /* v78: subtitle images made after the start (not before mp_selectplay) */
        if (v_sound == 2 && !av_checked && current_tick - run_t0 > cfg_av_stall * HZ && !pl_paused && !sc_active)
        {   /* v53: does the stock route run?  The VC's clock follows its sound output: no sound output = no clock = no picture */
            uint32_t e = av_vc_elapsed();
            av_checked = true;
            BLOG("  audio check (stock route) %d s in: VC elapsed %u ms, AAC frames sent %d (asked %d, %d B, last ts %u ms)", cfg_av_stall,
                 (unsigned)e, a_frames, a_req, a_bytes, (unsigned)a_last_ts);
            if (e != 0xffffffffu && e < 1000)
            {
                BLOG("  audio: the picture does not move with the VC's sound -> restarting with Rockbox's sound (sound=1)");
                v_fallback = true;
                break;
            }
            if (e != 0xffffffffu) { rate_e0 = e; rate_t0 = current_tick; rate_seeks = v_seeks; }
            if (a_frames == 0)
            {   int pct = av_vol_pct;
                BLOG("  audio: the VC plays the picture but never asked for sound -> Rockbox plays it (sound=1)");
                av_close(); v_sound = 1; av_open(bcm_play_path ? bcm_play_path : v_path); av_set_vol(pct);
            }
        }
        if (rate_t0 && current_tick - rate_t0 > 6 * HZ)
        {   /* v54: the VC's clock follows its sound output: 100 % = the sound plays at the right speed/pitch */
            if (!pl_paused && !sc_active && v_seeks == rate_seeks)
            {
                uint32_t e = av_vc_elapsed();
                long ms = (current_tick - rate_t0) * 1000 / HZ;
                if (e != 0xffffffffu && e > rate_e0)
                    BLOG("  audio: VC clock speed %ld.%ld %% of real time (%u ms in %ld ms)%s", (long)(e - rate_e0) * 100 / ms,
                         ((long)(e - rate_e0) * 1000 / ms) % 10, (unsigned)(e - rate_e0), ms,
                         (e - rate_e0) * 100 / ms < 96 ? " -- SLOW: the codec is not at the VC's sample rate (sound too deep)" : "");
                rate_t0 = 0;
            }
            else { rate_t0 = current_tick; rate_e0 = av_vc_elapsed(); rate_seeks = v_seeks; if (rate_e0 == 0xffffffffu) rate_t0 = 0; }
        }
        if (!TIME_BEFORE(current_tick, next_log))
        {
            if (ra_buf)
                BLOG("  read-ahead: %u KB ahead of the picture, %u bursts, %u restarts, %u KB read, drive %s", ra_hi - (ra_pos[0] > ra_pos[1] ? ra_pos[0] : ra_pos[1]) > ra_size ? 0 :
                     (ra_hi - (ra_pos[0] > ra_pos[1] ? ra_pos[0] : ra_pos[1])) / 1024, ra_bursts, ra_misses, ra_read_kb, ra_filling ? "reading" : "idle");
            BLOG("  video: get_frame=%d (+%d/2s) sent=%d (%d B) empty=%d eos=%d seeks=%d skipped=%d  cache misses: tables %u data %u",
                 v_getframes, v_getframes - gf_log, v_frames, v_bytes, v_empty, v_eos, v_seeks, v_skips, (unsigned)m4c_miss, (unsigned)m4d_miss);
            gf_log = v_getframes;
            if (v_sound == 2 && ma.ok)
                BLOG("  sound (VC): aac frames=%d (%d B) last ts=%u ms asked=%d eos=%d", a_frames, a_bytes, (unsigned)a_last_ts, a_req, a_eos);
            /* v47.1: backlight + power-off timer are now kept alive below, only while the video is not paused */
            next_log += 2 * HZ;
        }
        if (dump1 && !TIME_BEFORE(current_tick, dump1)) { dump1 = 0; if (cfg_dump_sram) bcm_vc_dump("play1", 0, (uint32_t)cfg_dump_len); if (cfg_dump_sdram) bcm_vc_dump("sd1", 0xC0000000u, 0x400000u); }
        if (dump2 && !TIME_BEFORE(current_tick, dump2)) { dump2 = 0; if (cfg_dump_sram) bcm_vc_dump("play2", 0, (uint32_t)cfg_dump_len); if (cfg_dump_sdram) bcm_vc_dump("sd2", 0xC0000000u, 0x400000u); }
        if (probe_tick && !TIME_BEFORE(current_tick, probe_tick))
        {
            probe_tick = 0;
            bcm_mid_probe();
        }
        if (gfx_front >= 0 && !TIME_BEFORE(current_tick, gfx_next)) { gfx_next = current_tick + HZ; bcm_gfx_present(); }
        if ((pw1 && !TIME_BEFORE(current_tick, pw1)) || (pw2 && !TIME_BEFORE(current_tick, pw2)))
        {
            char c[64]; int r2;
            if (pw1 && !TIME_BEFORE(current_tick, pw1)) pw1 = 0; else pw2 = 0;
            snprintf(c, sizeof c, "display_control 0 model=%d power=2", bcm_hw_model(0));
            bcm_cmd_t0 = current_tick; r2 = bcm_gencmd_watched(c, resp, sizeof resp, 3); bcm_log_text(c, r2, resp);
            BLOG("  >>> LOOK: power=2 re-sent during playback");
        }
        if (!TIME_BEFORE(current_tick, next_stat))
        {
            int r;
            bool full = nstat < 2 || nstat % 12 == 0;  /* v45/v57: full dumps at 5 and 15 s, then every 2 min; one line otherwise */
            nstat++;
            bcm_cmd_t0 = current_tick;
            r = bcm_gencmd_watched("mp_get_stats video", resp, sizeof resp, 1);
            if (full) bcm_log_text("mp_get_stats video", r, resp);
            else
            {
                uint32_t sq = pl_num(resp, "Disp seq_num="), dt = pl_num(resp, "Disp time="), mx = pl_num(resp, "max_time_ahead=");
                BLOG("  stats: Disp seq_num=%d Disp time=%d max_time_ahead=%d", (int)sq, (int)dt, (int)mx);
            }
            if (cfg_framedump && !framedumped)
            {
                /* v40: the decoder reports the picture it handed to the display ("Disp decoded_data=<VC address>",
                   320x240).  Save 0x30000 bytes from there so the frame can be rendered on a PC. */
                const char *q = strstr(resp, "Disp decoded_data=");
                if (q)
                {
                    unsigned long fa = 0; const char *h = q + 18;
                    while ((*h >= '0' && *h <= '9') || (*h >= 'a' && *h <= 'f') || (*h >= 'A' && *h <= 'F'))
                    { fa = fa * 16 + (unsigned long)(*h <= '9' ? *h - '0' : (*h | 0x20) - 'a' + 10); h++; }
                    if (fa >= 0xC0000000ul)
                    {
                        framedumped = 1;
                        BLOG("  frame dump: decoded picture at VC %08lx", fa);
                        bcm_vc_dump("frame", (uint32_t)fa, 0x30000);
                    }
                }
            }
            bcm_cmd_t0 = current_tick;
            r = bcm_gencmd_watched("mp_get_status", resp, sizeof resp, 1);
            if (full) bcm_log_text("mp_get_status", r, resp);
            else BLOG("  status: elapsed=%d buffering=%d paused=%s", (int)pl_num(resp, "elapsed="), (int)pl_num(resp, "buffering="),
                      strstr(resp, "paused=yes") ? "yes" : "no");
            if (v_sound == 2 && full)
            {   /* v53: the VC's own view of the sound */
                bcm_cmd_t0 = current_tick;
                r = bcm_gencmd_watched("mp_get_stats audio", resp, sizeof resp, 1);
                bcm_log_text("mp_get_stats audio", r, resp);
            }
            {   static int props_done;                 /* v38: what mplayer understood of our stream description, once */
                if (!props_done)
                {
                    static const char * const pc[] = { "mp_get_file_props", "mp_get_file_props video", "mp_get_stats audio" };
                    int k;
                    props_done = 1;
                    for (k = 0; k < 3; k++)
                    {
                        bcm_cmd_t0 = current_tick;
                        r = bcm_gencmd_watched(pc[k], resp, sizeof resp, 2);
                        bcm_log_text(pc[k], r, resp);
                    }
                }
            }
            next_stat += nstat < 2 ? 5 * HZ : 10 * HZ;    /* v57: every 10 s after the first two */
        }
        /* v45: keep polling without sleeping while the VC is pulling frames (a request within the last 50 ms);
           sleep a tick only when it has gone quiet (paused, or buffer full) */
        if (v_getframes != gf_last) { gf_last = v_getframes; last_req = current_tick; yield(); }
        else if (current_tick - last_req < HZ / 20) yield();
        else sleep(1);
    }
    bcm_video_running = false;                         /* v63 */
    sc_remote = false;
    bcm_sync_every = 0;
    av_close();
    av_boost(false);
#ifdef HAVE_BATTERY_SMOOTH_API
    powermgmt_set_load_hint(3, false);
#endif
    backlight_set_on_button_hold(global_settings.backlight_on_button_hold);
    bcm_map_probe();
}

/* v17: the stock firmware sends "pm_set_policy min" (rt 0x1b013c, called from the mp_selectplay builder at rt 0x1fa3f0
   and the mp_play builder at rt 0x1f8ea0, and from the TV-out code) right before those commands; v16 never did. */
static void bcm_pm_set(void)
{
    char resp[256]; int r;
    if (!cfg_pm) return;
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("pm_set_policy min", resp, sizeof resp, 3);
    bcm_log_text("pm_set_policy min", r, resp);
}
/* v27: the stock mp_selectplay path starts with "audio_enable 1" (function 0x1f90f4, called at 0x1fa3dc), then pm_set_policy min, then mp_selectplay. */
static void bcm_audio_enable(void)
{
    char resp[200]; int r;
    if (!cfg_audio) return;
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("audio_enable 1", resp, sizeof resp, 3);
    bcm_log_text("audio_enable 1", r, resp);
}
/* v28: the stock mp_selectplay path starts in rt 0x1f9158 with gencmd "mp_setvol 0" (string at 0x4fc2c0), BEFORE audio_enable. */
static void bcm_setvol(void)
{
    char resp[200]; int r;
    if (!cfg_setvol) return;
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("mp_setvol 0", resp, sizeof resp, 3);
    bcm_log_text("mp_setvol 0", r, resp);
}
static void bcm_freeze0(void)
{
    char resp[200]; int r;
    if (!cfg_freeze) return;
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("display_control 0 freeze=0", resp, sizeof resp, 3);
    bcm_log_text("display_control 0 freeze=0", r, resp);
}
static void bcm_pm_info(void)
{
    char resp[256]; int r;
    if (!cfg_pm) return;
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("power_management get_info", resp, sizeof resp, 3);
    bcm_log_text("power_management get_info", r, resp);
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("pm_get_status", resp, sizeof resp, 3);
    bcm_log_text("pm_get_status", r, resp);
}
static void bcm_step(const char *name)
{
    long end = current_tick + 30 * HZ;
    int b;
    if (!cfg_step) return;
    BLOG("STEP done: %s -- look at the screen, press any button to continue (30 s)", name);
    while (TIME_BEFORE(current_tick, end))
    {
        bcm_host_service();
        if (bcm_check_dead()) return;
        b = button_get_w_tmo(HZ / 50);
        if (b != BUTTON_NONE && !(b & BUTTON_REL))
            break;
    }
    BLOG("STEP continue: %s", name);
}

/* v12: display bring-up probe. Stock ARM firmware never sends vmcs_display_enable; the only display
   power/model command in the ARM image is "display_control <id> model=<1|2> power=<n>" (builder at rt
   0x164e9c; model = 2 when the SysInfo hw word is 0x000b00xx with xx >= 0x10, i.e. 5.5G -- inference).
   The display id (ctx+4) was NOT resolved, so try 0 then 1. Every reply (or error=... string) is logged.
   Short 6s budget each: the VC answers display commands quickly or not at all. */
/* v37: the stock model value (rt 0x265518): SysInfo word +0x84 (rt 0x265164 -> 0x2827b8; Rockbox reads the same word at NOR
   0x2084 = ROM 0x20002084): model = 2 if major == 0xb and minor >= 0x10, else 1. */
static int bcm_hw_model(int log)
{
    uint32_t hw = *(volatile uint32_t *)0x20002084;
    int m = ((hw >> 16) == 0xb && (hw & 0xffff) >= 0x10) ? 2 : 1;
    if (log) BLOG("SysInfo hw word (ROM 0x20002084) = %08lx -> stock display model %d", (unsigned long)hw, m);
    if (cfg_dc_model == -1) m = 3 - m;
    else if (cfg_dc_model > 0) m = cfg_dc_model;
    return m;
}
static void bcm_display_final(void)
{
    char cmd[64], resp[200]; int r;
    if (cfg_dc_final == 99 || bcm_io_dead()) return;
    if (cfg_v40)
    {
        /* decoded power state machine: the first power=2 after a boot turns the panel on and arms a flag, the second
           power=2 starts the GRAM stream (v39: the status screen appeared). */
        int k;
        for (k = 0; k < 2; k++)
        {
            snprintf(cmd, sizeof cmd, "display_control 0 model=%d power=2", bcm_hw_model(0));
            bcm_cmd_t0 = current_tick; r = bcm_gencmd_watched(cmd, resp, sizeof resp, 3); bcm_log_text(cmd, r, resp);
            bcm_hold_ms(1000);
        }
        if (gfx_front >= 0) bcm_gfx_present_m(0x101);
        BLOG("  >>> LOOK: the dark-blue STATUS SCREEN should be on now (it stays until mp_play)");
        bcm_hold_ms(2000);
        return;
    }
    snprintf(cmd, sizeof cmd, "display_control 0 model=%d power=%d", bcm_hw_model(0), cfg_dc_final);
    BLOG("---- v37 final display power command (after the UI layer exists)");
    if (cfg_dc_extra == 2)
    {
        char c2[64]; snprintf(c2, sizeof c2, "display_control 0 model=%d power=-2", bcm_hw_model(0));
        bcm_cmd_t0 = current_tick; r = bcm_gencmd_watched(c2, resp, sizeof resp, 3); bcm_log_text(c2, r, resp);
        bcm_hold_ms(1500);
    }
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched(cmd, resp, sizeof resp, 3);
    bcm_log_text(cmd, r, resp);
    if (cfg_dc_extra == 1)
    {
        bcm_cmd_t0 = current_tick; r = bcm_gencmd_watched("display_control 0 enabled=1", resp, sizeof resp, 3);
        bcm_log_text("display_control 0 enabled=1", r, resp);
    }
    BLOG("  >>> LOOK (A, 3 s): panel on? (v37 showed the OLD debug menu + 'booting' splash here)");
    bcm_hold_ms(3000);
    if (cfg_fbtest)
    {
        /* where does that old picture come from?  The legacy Rockbox FB at SRAM 0xE0000 still holds it after the VC reboot. */
        static uint16_t row[320];
        int x, y;
        for (y = 0; y < 240; y++)
        {
            for (x = 0; x < 320; x++) row[x] = (uint16_t)(((x / 20) & 1) ^ ((y / 20) & 1) ? 0x07E0 : 0xF81F);   /* green/magenta checkerboard */
            bcm_io_lock(); bcm_io_write(0xE0000 + 640 * (unsigned)y, row, sizeof row); bcm_io_unlock();
            if ((y & 31) == 31) bcm_host_service();
        }
        BLOG("  >>> LOOK (B, 3 s): green/magenta CHECKERBOARD written to SRAM 0xE0000 (no kick).  If you see it, the panel is fed from 0xE0000");
        bcm_hold_ms(3000);
        bcm_cmd_t0 = current_tick; r = bcm_gencmd_watched(cmd, resp, sizeof resp, 3); bcm_log_text(cmd, r, resp);
        BLOG("  >>> LOOK (C, 3 s): same power command re-sent after the checkerboard write");
        bcm_hold_ms(3000);
    }
    if (gfx_front >= 0)
    {
        static const int modes[4] = { 0x100, 0x101, 0x103, 0x109 };
        static const char * const mname[4] = { "stock (op1, op4, write, op3)", "+ op2 after op3", "+ op 0x10(img,1) before the write + op2",
                                                "B=0 instead of -50, + op2" };
        int k;
        for (k = 0; k < 4; k++)
        {
            BLOG("  >>> LOOK (D%d, 3 s): status screen presented with %s", k, mname[k]);
            bcm_gfx_present_m(modes[k]);
            bcm_hold_ms(3000);
        }
    }
    {
        /* v39: decoded lcd_power (vmcs.bin 0xf672): r0 = power + 2 indexes a byte jump table at 0xf744.
           power=-2: only sets state 2 (no panel access);  power=0: R002=0x400 + GPIO 0x30;  power=1: R100=0x0704 (standby);
           power=2: display-on ramp (R007 0x31 -> 0x72 -> 0x173); the FIRST time it only sets state 3 and flag [0xeb78+0x1c],
                    the second time it runs the +0x30 sequence and 0x10c9c (full 320x240 window + GRAM write start);
           power=4: +0x30 sequence + 0x10c9c directly.  So: power=2 twice, then power=4, each followed by a present. */
        static const int pw[3] = { 2, 4, 2 };
        int k;
        for (k = 0; k < 3; k++)
        {
            char c3[64];
            snprintf(c3, sizeof c3, "display_control 0 model=%d power=%d", bcm_hw_model(0), pw[k]);
            bcm_cmd_t0 = current_tick; r = bcm_gencmd_watched(c3, resp, sizeof resp, 3); bcm_log_text(c3, r, resp);
            if (gfx_front >= 0) bcm_gfx_present_m(0x101);
            BLOG("  >>> LOOK (E%d, 3 s): after power=%d + one status-screen present", k, pw[k]);
            bcm_hold_ms(3000);
        }
    }
}
static void bcm_display_probe(void)
{
    static const char * const dc[] = {
        "display_control 0 model=2 power=1",
        "display_control 1 model=2 power=1",
        "display_control 0 model=1 power=1",
    };
    char resp[256];
    int i, r;
    /* v31: the stock builder (rt 0x164e9c) maps its state argument to the power= value: state 0 -> 0, state 1 -> 2 (or -2 the
       first time, when obj[0xe] is set), states 2 and 3 -> 1.  We always sent power=1 only. */
    if (cfg_dc_seq == 1 || cfg_dc_seq == 2)
    {
        const char *c = (cfg_dc_seq == 1) ? "display_control 0 model=2 power=-2" : "display_control 0 model=2 power=2";
        bcm_cmd_t0 = current_tick;
        r = bcm_gencmd_watched(c, resp, sizeof resp, 3);
        bcm_log_text(c, r, resp);
    }
    {
        int m = bcm_hw_model(1);
        if (cfg_pm27)
        {
            bcm_cmd_t0 = current_tick;
            r = bcm_gencmd_watched("power_management set_policy manual 27", resp, sizeof resp, 3);   /* rt 0x1b0548: sent at every vmcs.bin boot */
            bcm_log_text("power_management set_policy manual 27", r, resp);
        }
        if (cfg_dc_noinit) { BLOG("dc_noinit: no display_control power command at bring-up (stock sends it later, after the UI layer)"); return; }
        if (m != 2)
        {
            char c[64]; snprintf(c, sizeof c, "display_control 0 model=%d power=1", m);
            bcm_cmd_t0 = current_tick;
            r = bcm_gencmd_watched(c, resp, sizeof resp, 3);
            bcm_log_text(c, r, resp);
            return;
        }
    }
    for (i = 0; i < (int)(sizeof dc / sizeof dc[0]) && (i == 0 || cfg_disp_all); i++)
    {
        bcm_cmd_t0 = current_tick;
        r = bcm_gencmd_watched(dc[i], resp, sizeof resp, 3);
        bcm_log_text(dc[i], r, resp);
    }
}

/* v19: mp_region as the stock firmware builds it (rt 0x1f9354): display=0, dest="x y w h", and mode=fill or
   mode=letterbox pixel="1 1" -- those are the ONLY mode strings in the firmware. v18's "dest=fullscreen mode=yuv422i"
   was answered with error=2 "bad parameters". */
static void bcm_region(const char *when)
{
    char cmd[96], resp[256]; int r;
    snprintf(cmd, sizeof cmd, "mp_region display=0 dest=\"0 0 %d %d\" mode=%s", cfg_region_w, cfg_region_h,
             cfg_region_mode ? "letterbox pixel=\"1 1\"" : "fill");
    BLOG("mp_region (%s)", when);
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched(cmd, resp, sizeof resp, 4);
    bcm_log_text(cmd, r, resp);
}

/* v55: TV output the stock way (rt 0x163b98 / 0x163b64, the "DtvDriverVC02" display driver, display 2):
     on:  mp_region display=2 dest=fullscreen mode=letterbox|fill ; display_control 2 dac=1 encoding=5 (NTSC) | 1 (PAL)
     off: display_control 2 dac=0
   The photo slideshow uses the same display: 720x480 (NTSC) / 720x576 (PAL), rt 0x1c77b8. */
static bool bcm_tv_dac;
static void bcm_tv_enable(bool on)
{
    char cmd[96], resp[256]; int r;
    if (on)
    {
        if (global_settings.bcm_tv_picture || !m4.width || !m4.height)
            snprintf(cmd, sizeof cmd, "mp_region display=2 dest=fullscreen mode=fill");
        else
        {   /* v56 letterbox computed here, for the TV's real shape: the raster is 720 x 480/576 shown as 4:3 (or 16:9 with
               Widescreen TV), so its pixels are not square.  v55's "dest=fullscreen mode=letterbox" assumed square pixels: a
               4:3 video then filled the 4:3 TV exactly as "fill" did (no visible difference) and was stretched on a 16:9 TV. */
            int tw = 720, th = global_settings.bcm_tv_signal ? 576 : 480, an = global_settings.bcm_tv_wide ? 16 : 4,
                ad = global_settings.bcm_tv_wide ? 9 : 3, dw, dh;
            dh = (int)((long)an * th * m4.height / ((long)ad * m4.width));     /* full width */
            dw = tw;
            if (dh > th) { dh = th; dw = (int)((long)m4.width * tw * ad / ((long)m4.height * an)); }
            dw &= ~1; dh &= ~1;
            snprintf(cmd, sizeof cmd, "mp_region display=2 dest=\"%d %d %d %d\" mode=fill", (tw - dw) / 2, (th - dh) / 2, dw, dh);
        }
        bcm_cmd_t0 = current_tick; r = bcm_gencmd_watched(cmd, resp, sizeof resp, 4); bcm_log_text(cmd, r, resp);
        snprintf(cmd, sizeof cmd, "display_control 2 dac=1 encoding=%d%s",
                 cfg_tv_enc >= 0 ? cfg_tv_enc : global_settings.bcm_tv_signal ? 1 : 5, cfg_tv_extra == 1 ? " svideo=1" : "");
        bcm_cmd_t0 = current_tick; r = bcm_gencmd_watched(cmd, resp, sizeof resp, 4); bcm_log_text(cmd, r, resp);
        bcm_tv_dac = true;
        /* v62: S-video needs nothing extra: vmcs.bin's display-2 defaults (copied from file offset 0x1f7f8 at boot, word 0 =
           0x01000088) already have the svideo bit (24, set by "display_control 2 svideo=", handler 0x5b7c) on, the same
           as RenderServer.bin's "2 power=1 svideo=1 mode=0" -- composite and S-video come out together; Apple's ARM code
           never sends svideo at all.  encoding 0..3 = PAL family, 4..5 = NTSC (handler 0x5b9e). */
        BLOG("  >>> TV OUT: %s %s%s -- composite + S-video together (dock connector pins 8 / 9-10, or the headphone jack's AV cable)",
             global_settings.bcm_tv_signal ? "PAL" : "NTSC", global_settings.bcm_tv_picture ? "fill" : "letterbox",
             global_settings.bcm_tv_wide ? " widescreen" : "");
    }
    else if (bcm_tv_dac)
    {
        bcm_cmd_t0 = current_tick; r = bcm_gencmd_watched("display_control 2 dac=0", resp, sizeof resp, 3);
        bcm_log_text("display_control 2 dac=0", r, resp);
        bcm_tv_dac = false;
    }
}

/* v78: hold MENU while a video plays: the picture moves between the TV and the iPod's screen without stopping
   (mp_region to the other display; the TV's DAC on / off).  Subtitles are made again for the new screen. */
static void bcm_region(const char *when);
static void bcm_out_switch(void)
{
    BLOG("  user: hold MENU -> the picture to the %s", bcm_tv ? "iPod screen" : "TV");
    st_hide();
    if (st_cv_h > 0) { core_unpin(st_cv_h); core_free(st_cv_h); }
    st_cv_h = -1; st_cv = NULL; st_wk = NULL; st_ext = NULL;
    if (st_font >= 0) font_unload(st_font);
    st_font = -1;
    st_ok = 0; st_win = 0; st_front = -1; st_nimg = 0; st_nshown = 0; st_shown_any = false; st_shown_msgv = -1;
    if (bcm_tv)
    {
        bcm_tv_enable(false);
        bcm_region("hold MENU: to the iPod screen");
        bcm_tv = false;
        backlight_set_brightness(MAX_BRIGHTNESS_SETTING);
        backlight_set_on_button_hold(2);
        backlight_on();
    }
    else
    {
        bcm_tv = true;
        bcm_tv_enable(true);
    }
    iap_dock_remember_tv(bcm_tv);
    st_toast(bcm_tv ? "TV" : "iPod");
    osd_show_for(3000);
}

static void bcm_dump(const char *what, unsigned vc_addr, unsigned len);   /* defined further down */
/* v20 diagnostics: after mp_selectplay the VC stops answering. Is its host interface dead, or only some tasks blocked? */
static void bcm_hi_dump(const char *tag)
{
    const struct bcm_host_diag *g = bcm_host_get_diag();
    BLOG("---- host-interface state: %s", tag);
    {   unsigned st[4]; uint32_t f8 = 0;
        bcm_io_status(st);
        bcm_io_lock(); bcm_io_read(0x1F8, &f8, 4); bcm_io_unlock();
        BLOG("bus status: BCM_CONTROL=%04x RD_ADDR=%04x ALT_CONTROL=%04x ALT_RD_ADDR=%04x;  VC[1F8]=%08x", st[0], st[1], st[2], st[3], (unsigned)f8); }
    bcm_dump("hdr +0x10 (signal bytes: tx_sig, rx_ack, ... +0x20 ack, +0x21 VC signal)", g->w1fc + 0x10, 0x20);
    {   /* v28: decode the four signal bytes. bit n = channel n (0 gencmd, 1 type5, 2 VCHR, 3 PDS, 4 graphics) */
        uint32_t a = 0, b = 0; unsigned txs, rxa, vca, vcs;
        bcm_io_lock(); bcm_io_read(g->w1fc + 0x10, &a, 4); bcm_io_read(g->w1fc + 0x20, &b, 4); bcm_io_unlock();
        txs = a & 0xff; rxa = (a >> 8) & 0xff; vca = b & 0xff; vcs = (b >> 8) & 0xff;
        BLOG("signals: host->VC tx_sig=%02x vs VC ack=%02x -> NOT YET ACKED by the VC: %02x;  VC->host sig=%02x vs our ack=%02x -> unanswered by us: %02x",
             txs, vca, txs ^ vca, vcs, rxa, vcs ^ rxa); }
    bcm_dump("ch0 (gencmd) descriptor", g->w1fc + g->dir[0], 0x50);
    bcm_dump("ch3 (PDS)    descriptor", g->w1fc + g->dir[3], 0x50);
}
static void bcm_diag_cmd(const char *cmd, int secs)
{
    char resp[200]; int r;
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched(cmd, resp, sizeof resp, secs);
    bcm_log_text(cmd, r, resp);
}
/* Find words in VC RAM that change by themselves (tick counters) while the VC is healthy, so that we can tell later
   whether the VC core still runs. */
#define BCM_CNT_MAX 24
static unsigned bcm_cnt_addr[BCM_CNT_MAX], bcm_cnt_n;
static void bcm_find_counters(void)
{
    static uint32_t a[1024], b[1024];
    unsigned base, i;
    bcm_cnt_n = 0;
    for (base = 0x1f000; base < 0x2c000 && bcm_cnt_n < BCM_CNT_MAX; base += 0x1000)
    {
        bcm_io_lock(); bcm_io_read(base, a, sizeof a); bcm_io_unlock();
        sleep(HZ / 20);
        bcm_io_lock(); bcm_io_read(base, b, sizeof b); bcm_io_unlock();
        for (i = 0; i < 1024 && bcm_cnt_n < BCM_CNT_MAX; i++)
            if (b[i] > a[i] && b[i] - a[i] < 100000u)
                bcm_cnt_addr[bcm_cnt_n++] = base + 4 * i;
    }
    BLOG("VC liveness: %u counter-like words found in VC RAM 0x1f000..0x2c000 (first: %05x %05x %05x)", bcm_cnt_n,
         bcm_cnt_n > 0 ? bcm_cnt_addr[0] : 0, bcm_cnt_n > 1 ? bcm_cnt_addr[1] : 0, bcm_cnt_n > 2 ? bcm_cnt_addr[2] : 0);
}
static int bcm_check_counters(const char *tag)
{
    unsigned i, adv = 0; uint32_t x, y;
    if (!bcm_cnt_n) return -1;
    for (i = 0; i < bcm_cnt_n; i++)
    {
        bcm_io_lock(); bcm_io_read(bcm_cnt_addr[i], &x, 4); bcm_io_unlock();
        sleep(HZ / 20);
        bcm_io_lock(); bcm_io_read(bcm_cnt_addr[i], &y, 4); bcm_io_unlock();
        if (y != x) adv++;
    }
    BLOG("VC liveness (%s): %u of %u counter words still changing -> VC core %s", tag, adv, bcm_cnt_n, adv ? "RUNNING" : "looks STOPPED");
    return (int)adv;
}

/* v23: where did the VC put our stream description? Search VC RAM (0x1f000..0x92000, covers the PDS buffers at 0x813c0)
   for two consecutive words (fourcc 'H264', bit rate) and print the neighbourhood: shows how far the VC's copy got and how
   it laid the fields out. */
static bool bcm_vc_stopped;
static void bcm_scan_vc_ram(uint32_t w0, uint32_t w1)
{
    static uint32_t buf[1024];
    unsigned base, i; int hits = 0;
    if (!w0 || !w1) return;
    BLOG("scanning VC RAM for the stream info words %08x %08x ...", (unsigned)w0, (unsigned)w1);
    for (base = 0x1f000; base < 0x92000 && hits < 4; base += 0x1000)
    {
        bcm_io_lock(); bcm_io_read(base, buf, sizeof buf); bcm_io_unlock();
        for (i = 0; i + 1 < 1024 && hits < 4; i++)
            if (buf[i] == w0 && buf[i + 1] == w1)
            {
                unsigned at = base + 4 * i, from = at >= 0x20 ? at - 0x20 : 0;
                BLOG("  HIT at VC 0x%05x", at);
                bcm_dump("  context", from, 0x60);
                hits++;
            }
    }
    if (!hits) BLOG("  no hit (the VC never copied it, or copied it differently)");
}

/* v78: start-up in parallel.  Opening the MP4 (tables, the read-ahead ring, the sound track) and finding / loading the
   subtitles need no VideoCore: ~1 s that used to wait until the VideoCore had booted, loaded mplayer.vll and powered the
   display (~1.7 s of mostly waiting for it).  In player mode a helper thread does them meanwhile; the playback probe takes
   the result.  Rockbox threads only switch at sleeps / yields / disk waits, which is where the main thread waits for the
   VideoCore. */
static struct { char path[MAX_PATH]; volatile int state; int mr; long ms; } prep;   /* state 0 none, 1 running, 2 done */
static long prep_stack[(16 * 1024) / sizeof(long)];
static void prep_thread(void)
{
    long t0 = current_tick;
    prep.mr = mp4_open(prep.path);
    if (prep.mr == 0) st_begin(prep.path);
    prep.ms = (current_tick - t0) * 1000 / HZ;
    prep.state = 2;
    thread_exit();
}
static void prep_start(const char *path)
{
    snprintf(prep.path, sizeof prep.path, "%s", path);
    prep.state = 1;
    if (!create_thread(prep_thread, prep_stack, sizeof prep_stack, 0, "bcm prepare"
                       IF_PRIO(, PRIORITY_USER_INTERFACE) IF_COP(, CPU)))
    {
        prep.state = 0;                                  /* no thread: done in order, as before */
        BLOG("start-up helper: no thread -- opening the video after the VideoCore start");
    }
}
static int prep_join(void)                               /* -100 = no helper ran */
{
    long t0 = current_tick;
    if (prep.state == 0) return -100;
    while (prep.state != 2) { bcm_host_service(); yield(); }
    BLOG("start-up helper: video + subtitles ready (%ld ms of work; waited %ld ms for it)", prep.ms, (current_tick - t0) * 1000 / HZ);
    prep.state = 0;
    return prep.mr;
}

static void bcm_playback_probe(void)
{
    char resp[256], video[MAX_PATH];
    int fd, r, i;

    if (bcm_play_path)                                 /* v47: player mode: the file chosen in the file browser */
    {
        snprintf(video, sizeof video, "%s", bcm_play_path);
        r = (int)strlen(video);
    }
    else
    {
        fd = open(BCM_TESTFILE_PATH, O_RDONLY);
        if (fd < 0) {
            BLOG("no %s -- skipping playback probe (create it with one line: the video's path)", BCM_TESTFILE_PATH);
            return;
        }
        r = read(fd, video, sizeof video - 1);
        close(fd);
    }
    if (r <= 0) { BLOG("testfile.txt is empty"); return; }
    video[r] = 0;
    for (i = 0; i < r; i++) if (video[i] == '\r' || video[i] == '\n') { video[i] = 0; break; }
    snprintf(v_path, sizeof v_path, "%s", video);
    BLOG("playback probe: test video = %s", video);

    int pre = prep_join();                             /* v78: opened meanwhile by the start-up helper */
    if (pre == -100)
    {
    BLOG("opening test video from Rockbox...");    /* progress marker: if the log stops right
                                                        after this line, the open() call itself
                                                        is what hung or crashed */
    {   /* just confirm Rockbox itself can open it -- this file is NOT sent to the VC (PDS pulls
           frames from the host on demand; there is no file-open step on the VC side for video) */
        int vfd = open(video, O_RDONLY);
        if (vfd < 0) { BLOG("cannot open %s from Rockbox -- check the path", video); return; }
        close(vfd);
    }
    BLOG("test video opened OK");
    }

    if (cfg_tv >= 0) bcm_tv = cfg_tv;
    if (!bcm_tv && (cfg_region_when & 1)) bcm_region("before mp_selectplay");

    {   int mr = pre != -100 ? pre : mp4_open(video);
        if (mr == 0)
        {
            BLOG("MP4 ok: %ux%u H.264 profile_idc=%u level_idc=%u nal_len=%u timescale=%u samples=%u SPS+PPS=%u B",
                 (unsigned)m4.width, (unsigned)m4.height, m4.profile, m4.level, m4.nal_len,
                 (unsigned)m4.timescale, (unsigned)m4.stsz_n, (unsigned)m4.ps_len);
            if (m4.profile != 66)
                BLOG("WARNING: profile_idc=%u, not Baseline (66). The VC may refuse CABAC/B-frames/High -- re-encode with -profile:v baseline", m4.profile);
            bcm_pds_set_ops(&pds_video_ops); v_live = true;
            if (pre == -100) st_begin(video);           /* v73: subtitles (v78: done by the helper when it ran) */
            {   /* v45: the overlay shows the file name without folder and extension */
                const char *bn = strrchr(video, '/'); char *dot;
                snprintf(pl_title, sizeof pl_title, "%s", bn ? bn + 1 : video);
                dot = strrchr(pl_title, '.'); if (dot && dot != pl_title) *dot = 0;
            }
            if (cfg_quiet && cfg_log_sync > 0)
            {
                BLOG("log: from here on flushed every %d s (quiet=1); PDS 0x42 traces after the first 24 are not written", cfg_log_sync);
                bcm_sync_every = cfg_log_sync * HZ;      /* v45: frames start flowing at mp_selectplay (pre-buffer) */
            }
            bcm_pds_map_stream(0, cfg_map);                 /* type 1 (video) -> buffer A (cap 0x10000, stock); audio would be B */
            v_sound = cfg_sound; v_fallback = false;
            if (v_sound == 2 && !ma.ok) { BLOG("no AAC track found -- no VC sound"); v_sound = cfg_sound ? 1 : 0; }
            if (v_sound == 2)
            {
                bcm_pds_map_stream(1, 1);                   /* v53: stream 1 (AAC) -> buffer B (cap 0x4000), like stock */
                BLOG("REAL VIDEO FEED enabled: H.264 stream 0 + AAC stream 1 (%u Hz, %u ch) -- the VideoCore plays the sound (stock route)",
                     (unsigned)ma.rate, (unsigned)ma.channels);
            }
            else BLOG("REAL VIDEO FEED enabled (video stream only; sound=%d)", v_sound);
        }
        else
        {
            BLOG("mp4_open failed (%d: -1 open, -2 empty, -3 no moov, -4 no avc1 track) -- empty-frame probe only", mr);
            bcm_pds_set_ops(&pds_probe_ops);
        }
    }
    if (bcm_tv) bcm_tv_enable(true);                   /* v55: the TV region replaces the LCD region; v56: after mp4_open (size) */
    bcm_step("mp_region");
again:
    bcm_setvol();                                      /* v28: stock order: mp_setvol 0, audio_enable 1, pm_set_policy min, mp_selectplay */
    bcm_audio_enable();                                /* v27: stock order: audio_enable 1, pm_set_policy min, mp_selectplay */
    bcm_pm_set();                                      /* stock order: pm_set_policy min, then mp_selectplay (cfg pm, default off since v20) */
    if (!cfg_fast) { bcm_find_counters(); bcm_hi_dump("just before mp_selectplay"); }
    bcm_cmd_t0 = current_tick;
    if (gfx_front >= 0) { BLOG("  >>> LOOK: sending mp_selectplay -- does the status screen turn WHITE now?"); bcm_gfx_present_m(0x101); }
    r = bcm_gencmd_watched("mp_selectplay passthru:test 0", resp, sizeof resp, cfg_sel_wait);
    bcm_log_text("mp_selectplay passthru:test 0", r, resp);
    bcm_cmd_t0 = current_tick;
    bcm_step("mp_selectplay");
    if (!cfg_fast) bcm_hi_dump("right after mp_selectplay");
    {
        int adv = cfg_fast ? 1 : bcm_check_counters("after mp_selectplay");
        if (adv == 0)
        {
            int k;
            BLOG("VC STOPPED after mp_selectplay: ringing the doorbell (BCM_CONTROL=0x31) 3 times to see whether it only sleeps");
            for (k = 0; k < 3; k++) { bcm_io_doorbell(); sleep(HZ / 20); }
            adv = bcm_check_counters("after 3 doorbells");
            bcm_scan_vc_ram(si_words[9], si_words[10]);
            if (adv == 0)
            {
                bcm_vc_stopped = true;
                BLOG("VC is dead for this payload variant -- ending the run early (log + reboot) so the next variant can be tried");
                bcm_diag_cmd("version", 1);
                bcm_hi_dump("final");
                return;
            }
        }
    }
    if (!cfg_fast)
    {
        bcm_diag_cmd("version", 2);
        bcm_diag_cmd("tasks", 2);
        bcm_diag_cmd("pm_get_status", 2);
        bcm_diag_cmd("mp_get_status", 2);
        bcm_hi_dump("after the four diagnostic commands");
    }
    if (cfg_region_when & 2) bcm_region("after mp_selectplay");
    if (cfg_pm_play) bcm_pm_set();                     /* v18: a second pm_set_policy min after selectplay never got a reply and blocked mp_play behind it */
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("mp_play", resp, sizeof resp, 10);
    bcm_log_text("mp_play", r, resp);
    if (cfg_region_when & 4) bcm_region("after mp_play");

    if (v_live)
    {
        bcm_video_run(cfg_play_secs > 0 ? cfg_play_secs : 6 * 3600);   /* v41: whole file by default (MENU stops) */
        BLOG("VIDEO RESULT: get_frame=%d frames_sent=%d bytes=%d empty=%d eos=%d stop=%d", v_getframes, v_frames, v_bytes, v_empty, v_eos, v_stop);
        if (v_fallback)
        {   /* v53: stop, then the same start-up again with one stream (Rockbox plays the sound) */
            long t;
            v_fallback = false; v_sound = 1;
            bcm_cmd_t0 = current_tick; r = bcm_gencmd_watched("mp_stop", resp, sizeof resp, 3); bcm_log_text("mp_stop", r, resp);
            for (t = current_tick + HZ / 2; TIME_BEFORE(current_tick, t); ) { bcm_host_service(); yield(); }
            m4_rewind(); m4.need_ps = true;
            v_stop = false; v_eos = 0; v_eos_tick = 0; v_frames = v_bytes = v_getframes = v_empty = v_seeks = v_skips = 0;
            pl_paused = false; tf_dir = 0;
            BLOG("restart: mp_selectplay again with the video stream only");
            goto again;
        }
        st_end();                                      /* v73 */
        m4_close();
    }
    else
    {
        long end = current_tick + 8*HZ;
        while (TIME_BEFORE(current_tick, end)) { bcm_host_service(); sleep(HZ/100); }
    }
    BLOG("PDS probe: starts=%d frames=%d seeks=%d stops=%d", pds_probe_starts, pds_probe_frames, pds_probe_seeks, pds_probe_stops);

    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("mp_get_status", resp, sizeof resp, 3);
    bcm_log_text("mp_get_status", r, resp);
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("mp_stop", resp, sizeof resp, 3);
    bcm_log_text("mp_stop", r, resp);
    bcm_tv_enable(false);                              /* v55 */
}

/* Hex dump of BCM memory into the log, 16 bytes per line. */
static void bcm_dump(const char *what, unsigned vc_addr, unsigned len)
{
    static unsigned char d[96] __attribute__((aligned(4)));
    unsigned i, j;
    if (len > sizeof d) len = sizeof d;
    bcm_io_lock();
    bcm_io_read(vc_addr, d, len);
    bcm_io_unlock();
    BLOG("%s @%08x:", what, vc_addr);
    for (i = 0; i < len; i += 16)
    {
        char line[80]; int n = snprintf(line, sizeof line, "  %04x:", i);
        for (j = 0; j < 16 && i + j < len; j++)
            n += snprintf(line + n, sizeof line - n, " %02x", d[i + j]);
        bcm_logline(line, n);
    }
}

/* Compare the image with what is now in VC RAM.  The running VC rewrites its own data, so a few
   differing blocks are normal; zero matching blocks means the image did not get there. */
static void bcm_verify_upload(unsigned len)
{
    static unsigned char rb[1024] __attribute__((aligned(4)));
    unsigned off, bad = 0, total = 0, shown = 0;
    for (off = 0; off + sizeof rb <= len; off += sizeof rb)
    {
        bcm_io_lock();
        bcm_io_read(off, rb, sizeof rb);
        bcm_io_unlock();
        total++;
        if (memcmp(rb, bcm_vmcs_buf + off, sizeof rb))
        {
            bad++;
            if (shown < 6) { BLOG("  block differs at 0x%x", off); shown++; }
        }
    }
    BLOG("upload verify: %u of %u KiB blocks differ from vmcs.bin", bad, total);
}

/* v47: the player can now run many times per boot (file browser), so the per-run state is reset here */
static void bcm_reset_run_state(void)
{
    int k;
    bcm_dead_seen = false; bcm_vc_stopped = false; bcm_trace_42 = 0;
    v_frames = v_bytes = v_getframes = v_empty = v_eos = v_seeks = v_skips = 0;
    v_stop = v_live = false; v_eos_tick = 0;
    tf_dir = 0; tf_fed = 0;
    gfx_front = -1; gfx_presents = 0; memset(gfx_win, 0, sizeof gfx_win); memset(gfx_black, 0, sizeof gfx_black);
    osd_ok = -1; osd_visible = false;
    for (k = 0; k < OSD_NS; k++) { osd_s[k].front = -1; memset(osd_s[k].win, 0, sizeof osd_s[k].win); osd_s[k].tvwin = 0; }
    pl_paused = false; pl_el_ms = 0; pl_dur_ms = 0; pl_target_ms = 0;
    sc_active = false; sc_btn = 0; mon_until = 0;
    av_have = false; av_state = 0; av_vol_until = 0;
    m4c_miss = m4d_miss = 0;
}
static bool bcm_run(const char *play_path);
static bool dbg_bcm_host(void) { return bcm_run(NULL); }
/* v47: play a video from the file browser and come back to it (the display goes back to Rockbox's own firmware) */
bool bcm_video_play(const char *path)
{   /* v55: Settings > General Settings > Video Out: On / Ask / Off (Ask = the stock "TV On / TV Off" choice) */
    int vo = global_settings.bcm_video_out, dock;
    bcm_video_next_request = 0;                        /* v63 */
    bcm_video_finished = false;
    iap_dock_ui();                                     /* v74: a dock that came while nothing looked: its settings first */
    dock = iap_dock_tv_choice();
    bcm_tv = vo == 0 || (vo == 3 && dock == 1);        /* v74: Auto = TV on a dock that wants it */
    if (vo == 1)
    {
        MENUITEM_STRINGLIST(tv_menu, ID2P(LANG_VIDEO_OUT), NULL, ID2P(LANG_TV_ON), ID2P(LANG_TV_OFF));
        int start = dock == 0 ? 1 : 0;                 /* v74: this dock's last answer */
        int sel = do_menu(&tv_menu, &start, NULL, false);
        if (sel != 0 && sel != 1) return false;        /* backed out: no video */
        bcm_tv = sel == 0;
        iap_dock_remember_tv(bcm_tv);
    }
    else if (bcm_tv && vo == 0 && dock != 1)
        splash(HZ, ID2P(LANG_VIDEO_ON_TV_NOTICE));    /* v78: Video Out is On: say so (the iPod's screen stays dark) */
    return bcm_run(path);
}
/* v63: the dock asked for the next / previous video: same TV choice, no question */
bool bcm_video_play_again(const char *path)
{
    bcm_video_next_request = 0;
    bcm_video_finished = false;
    return bcm_run(path);
}
bool bcm_video_is_video(const char *path)
{
    const char *e = strrchr(path, '.');
    return e && (!strcasecmp(e, ".mp4") || !strcasecmp(e, ".m4v") || !strcasecmp(e, ".mov"));
}
static bool bcm_run(const char *play_path)
{
    static char resp[1536];
    unsigned snap[4], boots;
    uint32_t w[4];
    int fd, r, i;
    unsigned vmcs_len;
    const struct bcm_host_diag *dg;
    static const char * const cmds[] = {
        "version", "commands", "tasks", "set_vll_dir /Resources/VideoCore/Library",
        "load_application mplayer.vll",   /* the VC opens the library over VCFS (trace shows the paths) */
        "tasks"                           /* v18: short reply. The LONG `commands` list (540 B through the 512 B gencmd ring)
                                             used to go here; it left the VC's gencmd task blocked, and every later command
                                             (this same `tasks`, pm_*, display_control, mp_*) timed out. It now runs at the
                                             very end (cfg cmds_end) once the signal-after-partial-read fix can be judged. */
    };

    bcm_play_path = play_path; bcm_player_mode = play_path != NULL;
    bcm_reset_run_state();
    bcm_log_open();
    bcm_t_start = current_tick;
    BLOG("bcm host test v79, built %s %s", __DATE__, __TIME__);
    if (bcm_player_mode)
        BLOG("dock: %s (TV choice %d), video out setting %d, TV %s", iap_dock_name() ? iap_dock_name() : "none",
             iap_dock_tv_choice(), global_settings.bcm_video_out, bcm_tv ? "on" : "off");
    bcm_sync_every = 2 * HZ;                             /* v46: flush the log at most every 2 s (was: every line) */
    BLOG("battery: %d mV, %d%%%s%s", battery_voltage(), battery_level(), bcm_player_mode ? "  -- PLAYER MODE: " : "", bcm_player_mode ? play_path : "");
#ifdef HAVE_BATTERY_SMOOTH_API
    {   /* v58 */
        struct battery_smooth_info bi;
        battery_smooth_info(&bi);
        BLOG("battery reading: %s, exact %d %%, compensated %d.%d %%, R %d mOhm, drops %d/%d/%d/%d mV (measured %d/%d/%d/%d)",
             bi.enabled ? "smooth" : "exact", bi.raw_percent, bi.est_permille / 10, bi.est_permille % 10, bi.resistance_mohm,
             bi.sag_mv[0], bi.sag_mv[1], bi.sag_mv[2], bi.sag_mv[3], bi.measured[0], bi.measured[1], bi.measured[2], bi.measured[3]);
        BLOG("battery curve: %s (Apple table %d, hw %08x); Apple-mode ADC %d, scale %s%d mV + %d.%03d mV/count after %d pairs; "
             "curve mV %d %d %d %d %d %d %d %d %d %d %d", bi.apple_curve ? "Apple" : "Rockbox", bi.apple_table, bi.hw_rev,
             bi.sub_raw, bi.cal_ok ? "" : "(not yet) ", bi.cal_a, bi.cal_b1000 / 1000, bi.cal_b1000 % 1000, bi.cal_n,
             bi.curve_mv[0], bi.curve_mv[1], bi.curve_mv[2], bi.curve_mv[3], bi.curve_mv[4], bi.curve_mv[5],
             bi.curve_mv[6], bi.curve_mv[7], bi.curve_mv[8], bi.curve_mv[9], bi.curve_mv[10]);
    }
#endif
    bcm_wd_last = current_tick; bcm_wd_on = 1;
    tick_add_task(bcm_wd_tick);
    bcm_load_cfg();
    if (!cfg_fast) bcm_sync_every = 0;
    else if (bcm_player_mode && cfg_log_sync > 0)
        bcm_sync_every = cfg_log_sync * HZ;            /* v78: a flush costs 0.1-0.6 s on a big FAT32 volume: not during start-up */
    bcm_apply_variant();
    if (bcm_player_mode && cfg_prep) prep_start(play_path);   /* v78 */

    /* v56: /.rockbox/VideoCore/ (all files in one folder) first, else the old /.rockbox/vc/Resources/VideoCore/... tree.
       vmcs.bin stays in RAM after the first video (it is re-sent to the BCM for every boot) */
    {
        static int vmcs_cached;
        static bool flat_cached;
        fd = -1;
        if (vmcs_cached > 0) { r = vmcs_cached; bcm_vc_flat = flat_cached; BLOG("vmcs.bin from RAM (%s)", bcm_vc_flat ? BCM_VC_FLAT : BCM_VC_ROOT); }
        else
        {
            fd = open(BCM_VC_FLAT "/vmcs.bin", O_RDONLY);
            bcm_vc_flat = fd >= 0;
            if (fd < 0) fd = open(BCM_VMCS_PATH, O_RDONLY);
            if (fd < 0)
            {
                BLOG("cannot open %s or %s", BCM_VC_FLAT "/vmcs.bin", BCM_VMCS_PATH);
                splash(HZ*3, "vmcs.bin not found");
                bcm_play_path = NULL;                  /* nothing was changed: no display restore needed */
                goto out;
            }
            r = read(fd, bcm_vmcs_buf, sizeof bcm_vmcs_buf);
            close(fd);
            BLOG("vmcs.bin read from %s", bcm_vc_flat ? BCM_VC_FLAT : BCM_VMCS_PATH);
            if (r > 0 && r < (int)sizeof bcm_vmcs_buf) { vmcs_cached = r; flat_cached = bcm_vc_flat; }
        }
    }
    if (r <= 0 || r >= (int)sizeof bcm_vmcs_buf)
    {
        BLOG("vmcs.bin read failed (%d)", r);
        goto out;
    }
    BLOG("vmcs.bin %d bytes", r);

    BLOG("booting the VC from vmcs.bin now (about 1 s; if the log stops here the boot hung: the watchdog reboots after %d s)", BCM_WD_SECS);
    if (!bcm_player_mode) splash(0, "Booting VideoCore from file...");   /* v47: the player starts without a message */
    BLOG("boot splash shown; calling bcm_use_vmcs_image");
    vmcs_len = (unsigned)r;
    r = bcm_use_vmcs_image(bcm_vmcs_buf, vmcs_len);
    boots = bcm_get_boot_info(snap);
    BLOG("bcm_use_vmcs_image = %d (0 ok, -1 no NOR vmcs, -2 BCM not rebooted); boots so far %u", r, boots);
    BLOG("VC[1F0..1FC] right after boot = %08x %08x %08x %08x",
         snap[0], snap[1], snap[2], snap[3]);
    /* v21: the v20 run that produced a one-line log booted "successfully" in 0.09 s (normal: 0.8 s) with
       VC[1F8]=fff70008 VC[1FC]=0 instead of 00000001 fe000ca0 -- the VC never started the image -- and then the
       marker-A splash below (legacy LCD path, unbounded bus waits) froze the iPod. A good boot has VC[1F8]==1 and a
       non-zero host-interface pointer in VC[1FC]. Retry the boot, and if it still is not good, leave without touching
       the LCD and reboot the iPod. */
    {
        int tries;
        for (tries = 0; tries < 2 && (r != 0 || snap[2] != 1 || snap[3] == 0); tries++)
        {
            BLOG("BAD BOOT (r=%d, VC[1F8]=%08x VC[1FC]=%08x): retrying the VC boot (%d/2)", r, snap[2], snap[3], tries + 1);
            sleep(HZ);
            r = bcm_use_vmcs_image(bcm_vmcs_buf, vmcs_len);
            boots = bcm_get_boot_info(snap);
            BLOG("retry: bcm_use_vmcs_image = %d; boots so far %u; VC[1F0..1FC] = %08x %08x %08x %08x", r, boots,
                 snap[0], snap[1], snap[2], snap[3]);
        }
        if (r != 0 || snap[2] != 1 || snap[3] == 0)
        {
            BLOG("BAD BOOT: the VideoCore did not start the uploaded image. Not touching the LCD; rebooting the iPod -- run the test again.");
            bcm_log_close();
            system_reboot();
        }
    }
    if (cfg_nolcd)
    {
        bcm_lcd_freeze(true);
        BLOG("nolcd: legacy LCD driver frozen (no splash markers, no lcd_tick kicks of VC[1F8] / BCM_CONTROL=0x31)");
    }
    else
        splash(HZ*2, "v26 marker A: VC booted from file");   /* legacy LCD path: visible or black? */

    bcm_io_lock(); bcm_io_read(0x1F0, w, 16); bcm_io_unlock();
    BLOG("VC[1F0..1FC] now              = %08x %08x %08x %08x",
         (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3]);

    if (cfg_fix1f8)
    {
        uint32_t one = 1, chk = 0;
        bcm_io_lock(); bcm_io_write(0x1F8, &one, 4); bcm_io_read(0x1F8, &chk, 4); bcm_io_unlock();
        BLOG("fix1f8: VC[1F8] written back to 1, reads %08x", (unsigned)chk);
    }
    {   unsigned st[4]; bcm_io_status(st);
        BLOG("bus status after boot: BCM_CONTROL=%04x RD_ADDR=%04x ALT_CONTROL=%04x ALT_RD_ADDR=%04x", st[0], st[1], st[2], st[3]); }

    if (!cfg_fast) bcm_verify_upload((unsigned)(sizeof bcm_vmcs_buf < 201376 ? sizeof bcm_vmcs_buf : 201376));   /* v56: 0.14 s, diagnostics only */
    bcm_dump("build string in VC RAM", 0x1F540, 64);

    for (i = 0; i < 3; i++)
    {
        r = bcm_host_attach();
        if (r == 0) break;
        sleep(HZ/10);
    }
    dg = bcm_host_get_diag();
    BLOG("bcm_host_attach = %d; ready_ok(VC[1F8]==1)=%d", r, dg->ready_ok);
    BLOG("directory:  %04x %04x %04x %04x %04x %04x %04x %04x",
         dg->dir[0], dg->dir[1], dg->dir[2], dg->dir[3], dg->dir[4], dg->dir[5], dg->dir[6], dg->dir[7]);
    BLOG("types:      %04x %04x %04x %04x %04x %04x %04x %04x",
         dg->type[0], dg->type[1], dg->type[2], dg->type[3], dg->type[4], dg->type[5], dg->type[6], dg->type[7]);
    for (i = 0; i < 8; i++)
        if (dg->dir[i])
            BLOG("ch%d: descriptor +%04x type %u  tx[%04x,%04x) rx[%04x,%04x)", i, dg->dir[i],
                 dg->type[i], (unsigned)dg->tx_start[i], (unsigned)dg->tx_end[i],
                 (unsigned)dg->rx_start[i], (unsigned)dg->rx_end[i]);
    if (dg->w1fc)
        bcm_dump("host-interface block (VC[1FC])", dg->w1fc, 64);
    bcm_dump("VC[0x200] (address used by the QEMU model)", 0x200, 64);
    if (r != 0)
        goto out;

    bcm_vfs_set_root(bcm_vc_flat ? BCM_VC_FLAT : BCM_VC_ROOT);
    bcm_vfs_set_flat(bcm_vc_flat);
    bcm_host_set_trace(bcm_trace_cb);
    bcm_host_set_vchr47_len((uint32_t)cfg_vchr47);
    bcm_host_set_pds_flags_legacy(cfg_flags_legacy);       /* v29: stock chunk flags unless flags_legacy=1 */       /* v28: 0 = stock zero-length reply to VCHR op 0x47 */
    for (i = 0; i < (int)(sizeof cmds / sizeof cmds[0]); i++)
    {
        if (cfg_fast && (!strcmp(cmds[i], "commands") || (i > 0 && !strcmp(cmds[i], "tasks")))) continue;   /* v46: diagnostics only */
        bcm_cmd_t0 = current_tick;
        /* v46 log: one VCFS read of mplayer.vll every 20 ms (two 10 ms poll sleeps each); the watched wait polls without sleeping */
        r = cfg_fast ? bcm_gencmd_watched(cmds[i], resp, sizeof resp, 10) : bcm_gencmd(cmds[i], resp, sizeof resp, 1000);
        bcm_log_text(cmds[i], r, resp);
    }

    {   /* listen for unsolicited traffic (VCFS/VCHR/VCPD requests) for 5 seconds */
        long end = current_tick + 5*HZ, t0 = current_tick, quiet = current_tick;
        uint32_t last_rx = bcm_host_get_stats()->rx_msgs;
        while (TIME_BEFORE(current_tick, end))
        {
            bcm_host_service();
            if (bcm_host_get_stats()->rx_msgs != last_rx) { last_rx = bcm_host_get_stats()->rx_msgs; quiet = current_tick; }
            /* v46 fast start: stop listening once the VC has been quiet for 0.3 s (mplayer.vll is loaded by then) */
            if (cfg_fast && (bcm_player_mode || (current_tick - t0 > HZ / 10 && current_tick - quiet > HZ / 10))) break;   /* v56: 0.1 s; v57: player: none (load_application has returned) */
            if (cfg_fast) yield(); else sleep(HZ/100);
        }
        const struct bcm_host_stats *st = bcm_host_get_stats();
        BLOG("rx=%u tx=%u vcfs=%u pds=%u bad_magic=%u unknown=%u",
             (unsigned)st->rx_msgs, (unsigned)st->tx_msgs, (unsigned)st->vcfs_ops,
             (unsigned)st->pds_ops, (unsigned)st->bad_magic, (unsigned)st->unknown_ops);
        BLOG("truncated=%u resynced=%u", (unsigned)st->truncated, (unsigned)st->resynced);
    }

    bcm_step("vmcs + mplayer.vll loaded");
    bcm_freeze0();                                       /* v28: optional, stock does "display_control <id> freeze=0" after the bring-up */
    bcm_pm_set();
    bcm_pm_info();
    bcm_step("pm_set_policy min / pm info");
    if (!cfg_skip_disp)
        bcm_display_probe();
    bcm_vde();
    if (cfg_freeze >= 2) bcm_freeze0();
    bcm_lcd_test();
    bcm_gfx_probe();
    bcm_display_final();
    if (gfx_front >= 0) bcm_gfx_present();
    bcm_step("display_control probe");
    if (!cfg_nolcd && !bcm_check_dead())
        splash(HZ*2, "v26 marker B: after display_control");
    bcm_playback_probe();

    if (cfg_cmds_end && !bcm_player_mode && !bcm_check_dead() && !bcm_vc_stopped)
    {
        char big[1024]; int cr;
        bcm_cmd_t0 = current_tick;
        cr = bcm_gencmd_watched("commands", big, sizeof big, 6);
        bcm_log_text("commands (end of run: oversized-reply check)", cr, big);
        { const struct bcm_host_stats *st = bcm_host_get_stats();
          BLOG("end stats: rx=%u tx=%u truncated=%u resynced=%u bad_magic=%u", st->rx_msgs, st->tx_msgs, st->truncated, st->resynced, st->bad_magic); }
    }
    if (v_live && !cfg_nolcd && !bcm_check_dead())
        splashf(HZ*3, "v26: sent %d video frames", v_frames);
out:
    if (prep.state != 0)
    {   /* v78: the start-up helper was not taken (the VideoCore start failed): let it finish, give its memory back */
        while (prep.state == 1) yield();
        prep.state = 0;
        st_end();
        m4_close();
    }
    if (bcm_player_mode)
    {   /* v47: back to the file browser: Rockbox's own display firmware is booted again, nothing is rebooted */
        int rr = 0;
        bcm_sync_every = 0;                            /* v47.1: every line below reaches the disk (v47 froze here, log lost) */
        bcm_wd_on = 0;
        tick_remove_task(bcm_wd_tick);
        if (bcm_play_path)
        {
            /* v56: panel off first, then the VideoCore (it may stop answering after power_control); backlight off before
               the panel is (a lit panel that loses its picture shows white).  bcm_restore_rom_vmcs() lights it again after
               the first complete picture from Rockbox's display firmware. */
            _backlight_hw_enable(false);
            if (!bcm_io_dead())
            {   /* v50: panel off the stock way (display state 0 -> power=0) before the BCM loses power; v49 sometimes left
                   the panel white when Rockbox's firmware took over (most often when leaving right after the start) */
                char c[48]; int pr2; long t1 = current_tick;
                snprintf(c, sizeof c, "display_control 0 model=%d power=0", bcm_hw_model(0));
                pr2 = bcm_gencmd_start(c);
                if (pr2 == 0) do { pr2 = bcm_gencmd_poll(resp, sizeof resp); if (pr2 != -3) break; yield(); } while (current_tick - t1 < HZ / 2);
                BLOG("player: %s -> %d (%ld ms)", c, pr2, (current_tick - t1) * 1000 / HZ);
            }
            /* stock VC shutdown (rt 0x289470): "power_control videocore 0", wait until VC[0x1F0] reads {0, magic},
               then GPIO 0x77 and 0x76 low (GPO32 bits 15, 14; in bcm_restore_rom_vmcs) */
            if (!bcm_io_dead())
            {
                uint32_t w1f0 = 0; long t0 = current_tick; int pr;
                /* v48 log: no reply within 2 s and VC[0x1F0] already read a5a50000 -> wait at most 0.3 s for the reply */
                /* v56: the v50..v55 loop ended after ONE poll (it tested pr == 0 after pr had become -3), so neither this nor the
                   panel-off command below was ever waited for (logs: "-> -3"): the BCM lost power with the panel still on ->
                   the occasional white screen.  Now both are waited for. */
                pr = bcm_gencmd_start("power_control videocore 0");
                if (pr == 0) do { pr = bcm_gencmd_poll(resp, sizeof resp); if (pr != -3) break; yield(); } while (current_tick - t0 < HZ * 3 / 10);
                BLOG("player: power_control videocore 0 -> %d", pr);
                do {
                    bcm_host_service();
                    bcm_io_lock(); bcm_io_read(0x1F0, &w1f0, 4); bcm_io_unlock();
                    if ((w1f0 & 0xffff) == 0) break;
                    sleep(HZ / 50);
                } while (current_tick - t0 < HZ);
                BLOG("player: VC[0x1F0] = %08x after %ld ms (stock waits for low half 0)", (unsigned)w1f0, (current_tick - t0) * 1000 / HZ);
            }
            bcm_host_detach();
            BLOG("player: GPO32 = %08lx -- powering the BCM down and booting Rockbox's display firmware", *(volatile unsigned long *)0x70000080);
            rr = bcm_restore_rom_vmcs();
            BLOG("player: display back to Rockbox (%d: 0 ok, -3 BCM did not boot, -2 no first update)", rr);
            backlight_set_brightness(global_settings.brightness);
            if (rr != 0 && rr != -2)
            {
                BLOG("player: the display did not come back -- rebooting instead of freezing");
                bcm_log_close();
                system_reboot();
            }
        }
        bcm_log_close();
        bcm_sync_every = 0; bcm_player_mode = false; bcm_play_path = NULL;
        backlight_on();
        lcd_update();                                  /* the frame buffer still holds the browser exactly as it was */
        return false;
    }
    if (bcm_dead_seen || (cfg_reboot_end && bcm_log_fd >= 0))
    {
        /* The LCD code spins forever on a dead BCM, so do NOT touch it: save the log and reboot cleanly. */
        BLOG(bcm_dead_seen ? "VC dead -- log saved, rebooting the iPod" : "test finished -- log saved, rebooting the iPod (cfg reboot_end=0 to stay)");
        bcm_log_close();
        system_reboot();
    }
    bcm_wd_on = 0;
    tick_remove_task(bcm_wd_tick);
    bcm_log_close();
    splash(HZ*2, "log: " BCM_LOG_PATH);
    return false;
}
#endif /* IPOD_VIDEO */

#ifdef IPOD_VIDEO
/* v64: exFAT / Rockbox-only research (README v64).  Writes /storage_report.txt: what the drive reports (ATA IDENTIFY),
   the raw MBR (and GPT header if there is one), each partition's first sector read in 512-byte AND 2048-byte units (which
   one holds the firmware-partition "]ih[" header / the FAT boot sector tells which units the MBR uses), the firmware
   partition's image directory, what Rockbox mounted.  Reads only.  (The flash ROM copy is dbg_flash_dump, v65.) */
static unsigned char sr_buf[4096] __attribute__((aligned(16)));
static void sr_hex(int fd, const unsigned char *b, int n)
{
    int i, j;
    for (i = 0; i < n; i += 16)
    {
        char line[96]; int k = snprintf(line, sizeof line, "  %03x:", i);
        for (j = 0; j < 16 && i + j < n; j++) k += snprintf(line + k, sizeof line - k, " %02x", b[i + j]);
        k += snprintf(line + k, sizeof line - k, "  ");
        for (j = 0; j < 16 && i + j < n; j++) line[k++] = b[i + j] >= 32 && b[i + j] < 127 ? b[i + j] : '.';
        line[k] = 0;
        fdprintf(fd, "%s\n", line);
    }
}
static int sr_read(sector_t lba512, int count)
{
    return storage_read_sectors(IF_MD(0,) lba512, count, sr_buf);
}
static uint32_t sr_le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static bool dbg_storage_report(void)
{
    int fd, i, rc;
    unsigned short *id = ata_get_identify();
    struct storage_info si;
    uint32_t pstart[4], psize[4]; unsigned char ptype[4];
    splash(0, "Writing /storage_report.txt ...");
    fd = creat("/storage_report.txt", 0666);
    if (fd < 0) { splash(HZ * 2, "cannot create /storage_report.txt"); return false; }
    fdprintf(fd, "storage report (bcm overlay v64)\n\n");
    storage_get_info(0, &si);
    fdprintf(fd, "storage_get_info: sector_size %u, num_sectors %llu (%llu MiB), vendor '%s' product '%s' rev '%s'\n",
             (unsigned)si.sector_size, (unsigned long long)si.num_sectors,
             (unsigned long long)si.num_sectors * si.sector_size >> 20, si.vendor ? si.vendor : "", si.product ? si.product : "",
             si.revision ? si.revision : "");
    if (id)
    {
        uint64_t lba28 = (uint32_t)id[60] | (uint32_t)id[61] << 16;
        uint64_t lba48 = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
        fdprintf(fd, "ATA IDENTIFY: LBA28 %llu, LBA48 supported %d, LBA48 %llu, word106 %04x, words117-118 %04x %04x, word209 %04x\n",
                 (unsigned long long)lba28, !!(id[83] & 0x400), (unsigned long long)lba48, id[106], id[117], id[118], id[209]);
        fdprintf(fd, "ATA IDENTIFY words 0..255:\n");
        sr_hex(fd, (const unsigned char *)id, 512);
    }
#ifdef MAX_VIRT_SECTOR_SIZE
    fdprintf(fd, "\nRockbox: disk sector multiplier %d (USB shows %d-byte sectors)\n", disk_get_sector_multiplier(IF_MD(0)),
             disk_get_sector_multiplier(IF_MD(0)) * 512);
#endif
    for (i = 0; i < NUM_VOLUMES; i++)
    {
        struct partinfo pi;
        if (disk_partinfo(i, &pi) == 0 && pi.type)
            fdprintf(fd, "Rockbox partinfo %d: type %02x start %llu size %llu (512-byte units after Rockbox's scaling)\n", i,
                     pi.type, (unsigned long long)pi.start, (unsigned long long)pi.size);
    }
    {
        sector_t sz = 0, fr = 0;
        volume_size(IF_MV(0,) &sz, &fr);
        fdprintf(fd, "volume 0: size %llu KiB, free %llu KiB, cluster %u bytes\n",
                 (unsigned long long)sz, (unsigned long long)fr, (unsigned)volume_get_cluster_size(IF_MV(0)));
    }
    rc = sr_read(0, 1);
    fdprintf(fd, "\nLBA 0 (MBR), read rc %d:\n", rc);
    sr_hex(fd, sr_buf, 512);
    for (i = 0; i < 4; i++)
    {
        unsigned char *e = sr_buf + 0x1be + 16 * i;
        ptype[i] = e[4]; pstart[i] = sr_le32(e + 8); psize[i] = sr_le32(e + 12);
        fdprintf(fd, "MBR entry %d: boot %02x type %02x start %lu size %lu  (= %llu MiB if 512-byte units, %llu MiB if 2048)\n", i, e[0],
                 ptype[i], (unsigned long)pstart[i], (unsigned long)psize[i], (unsigned long long)psize[i] >> 11,
                 (unsigned long long)psize[i] >> 9);
    }
    for (i = 0; i < 4; i++)
    {
        int u;
        if (!psize[i]) continue;
        if (ptype[i] == 0xee)
        {
            rc = sr_read(1, 1); fdprintf(fd, "\nGPT header at LBA 1 (512), rc %d:\n", rc); sr_hex(fd, sr_buf, 128);
            rc = sr_read(4, 1); fdprintf(fd, "GPT header at 2048-byte LBA 1, rc %d:\n", rc); sr_hex(fd, sr_buf, 128);
            continue;
        }
        for (u = 1; u <= 4; u *= 4)
        {
            rc = sr_read((sector_t)pstart[i] * u, 1);
            fdprintf(fd, "\npartition %d first sector read as %s-byte units (LBA %llu), rc %d:%s%s\n", i, u == 1 ? "512" : "2048",
                     (unsigned long long)pstart[i] * u, rc, !memcmp(sr_buf + 0x100, "]ih[", 4) ? "  <-- firmware partition header ]ih[" : "",
                     sr_buf[510] == 0x55 && sr_buf[511] == 0xaa ? "  <-- boot sector signature" : "");
            sr_hex(fd, sr_buf, i == 0 ? 0x120 : 0x80);
            if (!memcmp(sr_buf + 0x100, "]ih[", 4))
            {   /* the image directory: header +0x104 = directory offset, +0x10a = format version */
                uint32_t dir = sr_le32(sr_buf + 0x104) + 0x200;
                int ver = sr_buf[0x10a] | sr_buf[0x10b] << 8;
                fdprintf(fd, "  firmware partition: directory at +0x%lx, format version %d\n", (unsigned long)dir, ver);
                rc = sr_read((sector_t)pstart[i] * u + dir / 512, 1);
                sr_hex(fd, sr_buf + (dir % 512), 5 * 40);
            }
        }
    }
    close(fd);
    splash(HZ * 3, "Done: /storage_report.txt");
    return false;
}
/* v65: the flash ROM, copied 4 KB at a time through RAM (v64 wrote the 1 MB straight from 0x20000000 in one call and
   froze), with the progress on screen and in /flashrom_5g.txt (written after every 64 KB), so a stall shows exactly where */
static bool dbg_flash_dump(void)
{
    int fd, lf, n;
    unsigned off, nonff = 0, sum = 0;
    lf = creat("/flashrom_5g.txt", 0666);
    fd = creat("/flashrom_5g.bin", 0666);
    if (fd < 0 || lf < 0) { splash(HZ * 2, "cannot create /flashrom_5g.*"); if (fd >= 0) close(fd); if (lf >= 0) close(lf); return false; }
    fdprintf(lf, "flash ROM dump (bcm overlay v65): 0x20000000, %u bytes, 4 KB chunks\n", (unsigned)FLASH_SIZE);
    for (off = 0; off < FLASH_SIZE; off += sizeof sr_buf)
    {
        unsigned i;
        const volatile uint32_t *src = (const volatile uint32_t *)(0x20000000 + off);
        uint32_t *dst = (uint32_t *)sr_buf;
        if ((off & 0xffff) == 0)
        {
            splashf(0, "flash ROM %u / %u KB", off >> 10, (unsigned)FLASH_SIZE >> 10);
            fdprintf(lf, "reading 0x%08x\n", 0x20000000 + off);
            fsync(lf);
        }
        for (i = 0; i < sizeof sr_buf / 4; i++)
        {
            dst[i] = src[i];
            if (dst[i] != 0xffffffff) nonff++;
            sum += dst[i];
        }
        n = write(fd, sr_buf, sizeof sr_buf);
        if (n != (int)sizeof sr_buf)
        {
            fdprintf(lf, "write failed at 0x%x: %d\n", off, n);
            break;
        }
    }
    fdprintf(lf, "done: %u bytes, %u words not 0xffffffff, sum %08x, first words %08x %08x %08x %08x\n", off, nonff, sum,
             (unsigned)((uint32_t *)0x20000000)[0], (unsigned)((uint32_t *)0x20000000)[1], (unsigned)((uint32_t *)0x20000000)[2],
             (unsigned)((uint32_t *)0x20000000)[3]);
    close(fd); close(lf);
    splash(HZ * 3, "Done: /flashrom_5g.bin");
    return false;
}
#endif

/****** The menu *********/
static const struct {
    unsigned char *desc; /* string or ID */
    bool (*function) (void); /* return true if USB was connected */
} menuitems[] = {
#if defined(CPU_COLDFIRE) || \
    (defined(CPU_PP) && !(CONFIG_STORAGE & STORAGE_SD)) || \
    CONFIG_CPU == IMX31L || defined(CPU_TCC780X) || CONFIG_CPU == AS3525v2 || \
    CONFIG_CPU == AS3525 || CONFIG_CPU == RK27XX
        { "Dump ROM contents", dbg_save_roms },
#endif
#ifdef IPOD_VIDEO
        { "BCM host test", dbg_bcm_host },
        { "Storage report (exFAT prep)", dbg_storage_report },
        { "Dump flash ROM (exFAT prep)", dbg_flash_dump },
#endif
#if defined(CPU_COLDFIRE) || defined(CPU_PP) \
    || CONFIG_CPU == S3C2440 || CONFIG_CPU == IMX31L || CONFIG_CPU == AS3525 \
    || CONFIG_CPU == DM320 || defined(CPU_S5L87XX) || CONFIG_CPU == AS3525v2 \
    || CONFIG_CPU == RK27XX || CONFIG_CPU == JZ4760B
        { "View I/O ports", dbg_ports },
#endif
#if (CONFIG_RTC == RTC_PCF50605) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
        { "View PCF registers", dbg_pcf },
#endif
#if defined(HAVE_TSC2100) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
        { "TSC2100 debug", tsc2100_debug },
#endif
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        { "CPU frequency", dbg_cpufreq },
#endif
#if CONFIG_CPU == IMX31L
        { "DVFS/DPTC", __dbg_dvfs_dptc },
#endif
#if defined(IRIVER_H100_SERIES) && !defined(SIMULATOR)
        { "S/PDIF analyzer", dbg_spdif },
#endif
#if defined(CPU_COLDFIRE)
        { "Catch mem accesses", dbg_set_memory_guard },
#endif
        { "View OS stacks", dbg_os },
#ifdef __linux__
        { "View CPU stats", dbg_cpuinfo },
#endif
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
        { "View RAM info", view_ram_info },
#endif
#if (CONFIG_BATTERY_MEASURE != 0) && !defined(SIMULATOR)
        { "View battery", view_battery },
#endif
#ifndef APPLICATION
        { "Screendump", dbg_screendump },
#endif
        { "Skin Engine RAM usage", dbg_skin_engine },
#if ((CONFIG_PLATFORM & PLATFORM_NATIVE) || defined(SONY_NWZ_LINUX) || defined(HIBY_LINUX) || defined(FIIO_M3K_LINUX)) && !defined(SIMULATOR)
        { "View HW info", dbg_hw_info },
#endif
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
        { "View partitions", dbg_partitions },
#endif
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
        { "View disk info", dbg_disk_info },
#if (CONFIG_STORAGE & STORAGE_ATA)
        { "Dump ATA identify info", dbg_identify_info},
#ifdef HAVE_ATA_SMART
        { "View/Dump S.M.A.R.T. data", dbg_ata_smart},
#endif
#endif
#endif
        { "Metadata log", dbg_metadatalog },
#ifdef HAVE_DIRCACHE
        { "View dircache info", dbg_dircache_info },
#endif
#ifdef HAVE_TAGCACHE
        { "View database info", dbg_tagcache_info },
#endif
        { "View buffering thread", dbg_buffering_thread },
#ifdef PM_DEBUG
        { "pm histogram", peak_meter_histogram},
#endif /* PM_DEBUG */
#ifdef BUFLIB_DEBUG_PRINT
        { "View buflib allocs", dbg_buflib_allocs },
#endif
#ifndef SIMULATOR
#if CONFIG_TUNER
        { "FM Radio", dbg_fm_radio },
#endif
#endif
#if defined(HAVE_EEPROM) && !defined(HAVE_EEPROM_SETTINGS)
        { "Write back EEPROM", dbg_write_eeprom },
#endif
#if CONFIG_USBOTG == USBOTG_ISP1583
        { "View ISP1583 info", dbg_isp1583 },
#endif
#ifdef ROCKBOX_HAS_LOGF
        {"Show Log File", logfdisplay },
        {"Dump Log File", logfdump },
#endif
#if defined(HAVE_USBSTACK)
#if defined(ROCKBOX_HAS_LOGF) && defined(USB_ENABLE_SERIAL)
        {"USB Serial driver (logf)", toggle_usb_serial },
#endif
#if defined(USB_ENABLE_AUDIO)
        {"USB-DAC", dbg_usb_audio},
#endif
#endif /* HAVE_USBSTACK */
#ifdef CPU_BOOST_LOGGING
        {"Show cpu_boost log",cpu_boost_log},
        {"Dump cpu_boost log",cpu_boost_log_dump},
#endif
#if (defined(HAVE_WHEEL_ACCELERATION) && (CONFIG_KEYPAD==IPOD_4G_PAD) \
     && !defined(IPOD_MINI) && !defined(SIMULATOR))
        {"Debug scrollwheel", dbg_scrollwheel },
#endif
#if defined(IPOD_ACCESSORY_PROTOCOL)
        {"Debug IAP", dbg_iap },
#endif
        {"Talk engine stats", dbg_talk },
#if defined(HAVE_BOOTDATA) && !defined(SIMULATOR)
        {"Boot data", dbg_boot_data },
#endif

#if defined(HAVE_DEVICEDATA)// && !defined(SIMULATOR)
        {"Device data", dbg_device_data },
#endif

#if defined(IPOD_6G) && !defined(SIMULATOR)
        {"View SysCfg", dbg_syscfg },
        {"Dump bootflash to file", dbg_bootflash_dump },
#endif
};

static int menu_action_callback(int btn, struct gui_synclist *lists)
{
    int selection = gui_synclist_get_sel_pos(lists);
    if (btn == ACTION_STD_OK)
    {
        FOR_NB_SCREENS(i)
           viewportmanager_theme_enable(i, false, NULL);
        menuitems[selection].function();
        btn = ACTION_REDRAW;
        FOR_NB_SCREENS(i)
            viewportmanager_theme_undo(i, false);
    }
    else if (btn == ACTION_STD_CONTEXT)
    {
        MENUITEM_STRINGLIST(menu_items, "Debug", NULL, ID2P(LANG_ADD_TO_FAVES));
        if (do_menu(&menu_items, NULL, NULL, false) == 0)
            shortcuts_add(SHORTCUT_DEBUGITEM, menuitems[selection].desc);
        return ACTION_STD_CANCEL;
    }
    return btn;
}

static const char* menu_get_name(int item, void * data,
                                    char *buffer, size_t buffer_len)
{
    (void)data; (void)buffer; (void)buffer_len;
    return menuitems[item].desc;
}

static int menu_get_talk(int item, void *data)
{
    (void)data;
    if (global_settings.talk_menu && menuitems[item].desc)
    {
        talk_number(item + 1, true);
        talk_id(VOICE_PAUSE, true);
#if 0 /* no debug items currently have lang ids */
        long id = P2ID((const unsigned char *)(menuitems[item].desc));
        if(id>=0)
            talk_id(id, true);
        else
#endif
        talk_spell(menuitems[item].desc, true);
     }
    return 0;
}

int debug_menu(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, "Debug", ARRAYLEN(menuitems), NULL);
    info.action_callback = menu_action_callback;
    info.get_name        = menu_get_name;
    info.get_talk        = menu_get_talk;
    return (simplelist_show_list(&info)) ? 1 : 0;
}

bool run_debug_screen(char* screen)
{
    for (unsigned i=0; i<ARRAYLEN(menuitems); i++)
        if (!strcasecmp(screen, menuitems[i].desc))
        {
            FOR_NB_SCREENS(j)
               viewportmanager_theme_enable(j, false, NULL);
            menuitems[i].function();
            FOR_NB_SCREENS(j)
                viewportmanager_theme_undo(j, false);
            return true;
        }

    return false;
}
