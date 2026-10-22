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
extern int  bcm_use_vmcs_image(const void *img, unsigned len);
extern int  bcm_io_dead(void);        /* v16: host bus wait timed out -> the VC stopped answering */
extern void bcm_io_doorbell(void);    /* v23: BCM_CONTROL = 0x31, the wake/notify write */
extern void bcm_io_status(unsigned v[4]);   /* v24: bus status registers */
extern unsigned bcm_get_boot_info(unsigned snap[4]);

#define BCM_VC_ROOT    "/.rockbox/vc"
#define BCM_VMCS_PATH  BCM_VC_ROOT "/Resources/VideoCore/Boot/vmcs.bin"
#define BCM_LOG_PATH   "/bcm_host_log.txt"
#define BCM_TESTFILE_PATH BCM_VC_ROOT "/../../testfile.txt"   /* -> /.rockbox/testfile.txt: first line = video path */

static unsigned char bcm_vmcs_buf[204800] __attribute__((aligned(4))); /* vmcs.bin is 201376 B */
static int bcm_log_fd = -1;

static long bcm_t_start;
static char bcm_last_line[72];
static long bcm_last_tick;
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
    fsync(bcm_log_fd);                 /* keep the log even if the VideoCore hangs and we must hard-reset */
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
static void bcm_trace_cb(const char *tag, uint32_t a, uint32_t b, const char *s)
{
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
    long t0 = current_tick, last_beat = t0;
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
        sleep(HZ / 20);
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
    uint32_t stts, stts_n, stsc, stsc_n, stsz, stsz_fixed, stsz_n, stco, stco_n;
    bool     co64;
    uint32_t idx, chunk, s_in_chunk, off_in_chunk, stsc_i, spc, next_first;   /* sample cursor */
    uint32_t stts_i, stts_left, stts_delta; uint64_t dts;
} m4 = { .fd = -1 };

static uint32_t m4_be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static bool m4_rd(uint32_t off, void *buf, uint32_t n)
{
    if (lseek(m4.fd, (off_t)off, SEEK_SET) != (off_t)off) return false;
    return read(m4.fd, buf, n) == (ssize_t)n;
}
static uint32_t m4_u32(uint32_t off) { uint8_t b[4]; return m4_rd(off, b, 4) ? m4_be32(b) : 0; }
static uint32_t m4_u16(uint32_t off) { uint8_t b[2]; return m4_rd(off, b, 2) ? (uint32_t)((b[0]<<8)|b[1]) : 0; }
static uint32_t m4_u8(uint32_t off)  { uint8_t b[1]; return m4_rd(off, b, 1) ? b[0] : 0; }

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

static void m4_close(void) { if (m4.fd >= 0) close(m4.fd); m4.fd = -1; }
static uint32_t m4_dur_ms, m4_br;            /* v20: duration (ms) and average bit rate (bit/s) of the whole file */

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
    if (size == 0 || size > M4_RAW_MAX || !m4_rd(off, m4_raw, size)) return -1;
    for (p = 0; p + m4.nal_len <= size; )                               /* pass 1: is there an IDR? */
    {
        n = 0; for (k = 0; k < m4.nal_len; k++) n = (n << 8) | m4_raw[p + k];
        if (n == 0 || p + m4.nal_len + n > size) break;
        if ((m4_raw[p + m4.nal_len] & 0x1f) == 5) idr = true;
        p += m4.nal_len + n;
    }
    if ((first || idr) && m4.ps_len) { memcpy(m4_out, m4.ps, m4.ps_len); o = m4.ps_len; }
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
            region_when bitmask of when to send mp_region: 1 before mp_selectplay, 2 after it, 4 after mp_play (default 3)
            region_mode 0 = mode=fill, 1 = mode=letterbox pixel="1 1"  (the only two strings in the stock firmware)
            region_w / region_h  destination size (default 320 x 240)
            variant N = a preset of the 0x60 experiments below (0..5), or  variant=auto : use /.rockbox/bcm_variant.cnt, which
                     the test increments on every run (so just run the test again and again and send all the logs)
            fix1f8   1 (default) = write 1 back into VC[0x1F8] after boot. The stock attach (rt 0x288058) requires VC[0x1F8]==1 and
                     never writes it; Rockbox's own first LCD update (lcd_awake) leaves 0xffff0000 there (the "now" log line)
            si_extra 0 none / 1 raw avcC record / 2 SPS+PPS (Annex B) as the "extradata" of the 0x60 stream block
            si_b20   the byte at [s+0x20] (stock always 1)    noreply 1 = do NOT answer the VC's 0x41 start    minimal 1 = 0x60 with zero streams
            reboot_end 1 = when the test is over, close the log and reboot the iPod (default 1): the new VC image has no
                     legacy LCD protocol, so Rockbox's own screen is dead afterwards and a second run in the same power
                     session booted the VC badly (v20 log)
            pm_play 1 = send pm_set_policy min before mp_play too (default 0: the second one hung the VC in v18)
            disp_all 1 = also try display_control 1 / model=1 (default 0: only "display_control 0 model=2 power=1") */
static int cfg_si_w8 = -1, cfg_si_dur = -1, cfg_si_hw = 0, cfg_si_dim = 1, cfg_si_w1 = 0, cfg_si_last = 0, cfg_si_count = 1;
static int cfg_f2 = 2, cfg_map = 0, cfg_reply_ms = 0, cfg_skip_disp = 0, cfg_sel_wait = 12;
static int cfg_pm = 0, cfg_step = 0, cfg_cmds_end = 1, cfg_reboot_end = 1;
static int cfg_si_extra = 0, cfg_si_b20 = 1, cfg_noreply = 0, cfg_minimal = 0, cfg_variant = -1, cfg_fix1f8 = 1;
static int cfg_region_when = 1, cfg_region_mode = 0, cfg_region_w = 320, cfg_region_h = 240, cfg_pm_play = 0, cfg_disp_all = 0;
static void bcm_load_cfg(void)
{
    char buf[512], *p, *nl, *eq;
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
        else BLOG("cfg: unknown key '%s'", p);
    }
    BLOG("cfg2: region_when=%d region_mode=%d region=%dx%d pm_play=%d disp_all=%d", cfg_region_when, cfg_region_mode, cfg_region_w, cfg_region_h, cfg_pm_play, cfg_disp_all);
    BLOG("cfg: si_w8=%d si_dur=%d si_hw=%d si_dim=%d si_w1=%d si_last=%d si_count=%d f2=%d map=%d reply_ms=%d skip_disp=%d sel_wait=%d pm=%d step=%d cmds_end=%d",
         cfg_si_w8, cfg_si_dur, cfg_si_hw, cfg_si_dim, cfg_si_w1, cfg_si_last, cfg_si_count, cfg_f2, cfg_map, cfg_reply_ms, cfg_skip_disp, cfg_sel_wait, cfg_pm, cfg_step, cfg_cmds_end);
}

/* v22: the v21 log shows the VC core stops right after it consumes our 0x60 reply (selectplay is answered within 1 ms, then
   nothing: no further gencmd is read, its ack bits for ch0/ch2/ch3 never toggle, none of 6 RAM tick counters moves).
   Which part of the payload kills it is unknown, so each run can try one preset. */
#define BCM_NVARIANTS 6
static const char * const bcm_variant_name[BCM_NVARIANTS] = {
    "0: baseline (as v21: no extradata, b20=1)",
    "1: extradata = raw avcC record",
    "2: extradata = SPS+PPS Annex B",
    "3: raw avcC extradata + byte[s+0x20]=0",
    "4: do NOT answer the 0x41 (does the VC survive without our 0x60?)",
    "5: 0x60 with zero streams (all-zero payload)"
};
static void bcm_apply_variant(void)
{
    int v = cfg_variant;
    if (v == 99)                                   /* auto: counter file */
    {
        char c = '0'; int fd = open("/.rockbox/bcm_variant.cnt", O_RDONLY);
        if (fd >= 0) { if (read(fd, &c, 1) != 1) c = '0'; close(fd); }
        v = (c >= '0' && c <= '9') ? c - '0' : 0;
        v %= BCM_NVARIANTS;
        c = (char)('0' + (v + 1) % BCM_NVARIANTS);
        fd = open("/.rockbox/bcm_variant.cnt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { write(fd, &c, 1); close(fd); }
    }
    if (v < 0) return;
    if (v >= BCM_NVARIANTS) v = 0;
    cfg_si_extra = 0; cfg_si_b20 = 1; cfg_noreply = 0; cfg_minimal = 0;
    switch (v)
    {
    case 1: cfg_si_extra = 1; break;
    case 2: cfg_si_extra = 2; break;
    case 3: cfg_si_extra = 1; cfg_si_b20 = 0; break;
    case 4: cfg_noreply = 1; break;
    case 5: cfg_minimal = 1; break;
    default: break;
    }
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
    si_words[i++] = (uint32_t)cfg_si_last;
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
static bool v_stop, v_live;
static long v_eos_tick;
static void v_start(int id, uint32_t w1)
{
    BLOG("  PDS start: object=%d w1=%08x", id, (unsigned)w1);
    m4_rewind();
    if (cfg_reply_ms > 0) sleep(cfg_reply_ms * HZ / 1000 ? cfg_reply_ms * HZ / 1000 : 1);
    /* v15: the 0x60 stream description is the REPLY to this 0x41 (seq echoed) -- the VC waits for it. */
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
    if (want != -1 && want != 0)                        /* not the video stream (assumed 0 or "any") */
    {
        v_empty++; bcm_pds_send_empty();
        if (v_empty <= 4) BLOG("  -> not video (want=%d): sent empty", want);
        return;
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
    if (v_frames <= 12 || v_frames % 30 == 0 || r < 0)
        BLOG("  frame #%d sample=%u len=%d key=%d ts=%ums send=%d", v_frames, (unsigned)m4.idx, n, key, (unsigned)ts, r);
}
static void v_seek(int id, uint32_t pos)
{
    v_seeks++;
    BLOG("  PDS seek: stream=%d pos=%u -> %s", id, (unsigned)pos, pos == 0 ? "rewind" : "ignored (units unknown)");
    if (pos == 0) m4_rewind();
}
static void v_stopcb(void) { BLOG("  PDS stop (frames sent so far: %d)", v_frames); v_stop = true; }
static const struct bcm_pds_ops pds_video_ops = { v_start, v_get_frame, v_seek, v_stopcb };

/* Service loop while the VC plays: ~100 Hz, status line every 2 s, mp_get_stats every 5 s. */
static void bcm_video_run(int secs)
{
    long end = current_tick + secs * HZ, next_log = current_tick + 2 * HZ, next_stat = current_tick + 5 * HZ;
    char resp[256];
    while (TIME_BEFORE(current_tick, end) && !v_stop)
    {
        bcm_host_service();
        if (bcm_check_dead()) break;
        if (v_eos_tick && current_tick - v_eos_tick > 4 * HZ) break;
        if (!TIME_BEFORE(current_tick, next_log))
        {
            BLOG("  video: get_frame=%d sent=%d (%d B) empty=%d eos=%d seeks=%d skipped=%d",
                 v_getframes, v_frames, v_bytes, v_empty, v_eos, v_seeks, v_skips);
            next_log += 2 * HZ;
        }
        if (!TIME_BEFORE(current_tick, next_stat))
        {
            int r;
            bcm_cmd_t0 = current_tick;
            r = bcm_gencmd_watched("mp_get_stats video", resp, sizeof resp, 1);
            bcm_log_text("mp_get_stats video", r, resp);
            bcm_cmd_t0 = current_tick;
            r = bcm_gencmd_watched("mp_get_status", resp, sizeof resp, 1);
            bcm_log_text("mp_get_status", r, resp);
            next_stat += 5 * HZ;
        }
        sleep(HZ / 100);
    }
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
static void bcm_display_probe(void)
{
    static const char * const dc[] = {
        "display_control 0 model=2 power=1",
        "display_control 1 model=2 power=1",
        "display_control 0 model=1 power=1",
    };
    char resp[256];
    int i, r;
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

static void bcm_playback_probe(void)
{
    char resp[256], video[192];
    int fd, r, i;

    fd = open(BCM_TESTFILE_PATH, O_RDONLY);
    if (fd < 0) {
        BLOG("no %s -- skipping playback probe (create it with one line: the video's path)", BCM_TESTFILE_PATH);
        return;
    }
    r = read(fd, video, sizeof video - 1);
    close(fd);
    if (r <= 0) { BLOG("testfile.txt is empty"); return; }
    video[r] = 0;
    for (i = 0; i < r; i++) if (video[i] == '\r' || video[i] == '\n') { video[i] = 0; break; }
    BLOG("playback probe: test video = %s", video);

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

    if (cfg_region_when & 1) bcm_region("before mp_selectplay");

    {   int mr = mp4_open(video);
        if (mr == 0)
        {
            BLOG("MP4 ok: %ux%u H.264 profile_idc=%u level_idc=%u nal_len=%u timescale=%u samples=%u SPS+PPS=%u B",
                 (unsigned)m4.width, (unsigned)m4.height, m4.profile, m4.level, m4.nal_len,
                 (unsigned)m4.timescale, (unsigned)m4.stsz_n, (unsigned)m4.ps_len);
            if (m4.profile != 66)
                BLOG("WARNING: profile_idc=%u, not Baseline (66). The VC may refuse CABAC/B-frames/High -- re-encode with -profile:v baseline", m4.profile);
            bcm_pds_set_ops(&pds_video_ops); v_live = true;
            bcm_pds_map_stream(0, cfg_map);                 /* type 1 (video) -> buffer A (cap 0x10000, stock); audio would be B */
            BLOG("REAL VIDEO FEED enabled (video stream only, no audio)");
        }
        else
        {
            BLOG("mp4_open failed (%d: -1 open, -2 empty, -3 no moov, -4 no avc1 track) -- empty-frame probe only", mr);
            bcm_pds_set_ops(&pds_probe_ops);
        }
    }
    bcm_step("mp_region");
    bcm_pm_set();                                      /* stock order: pm_set_policy min, then mp_selectplay (cfg pm, default off since v20) */
    bcm_find_counters();
    bcm_hi_dump("just before mp_selectplay");
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("mp_selectplay passthru:test 0", resp, sizeof resp, cfg_sel_wait);
    bcm_log_text("mp_selectplay passthru:test 0", r, resp);
    bcm_cmd_t0 = current_tick;
    bcm_step("mp_selectplay");
    bcm_hi_dump("right after mp_selectplay");
    {
        int adv = bcm_check_counters("after mp_selectplay");
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
    bcm_diag_cmd("version", 2);
    bcm_diag_cmd("tasks", 2);
    bcm_diag_cmd("pm_get_status", 2);
    bcm_diag_cmd("mp_get_status", 2);
    bcm_hi_dump("after the four diagnostic commands");
    if (cfg_region_when & 2) bcm_region("after mp_selectplay");
    if (cfg_pm_play) bcm_pm_set();                     /* v18: a second pm_set_policy min after selectplay never got a reply and blocked mp_play behind it */
    bcm_cmd_t0 = current_tick;
    r = bcm_gencmd_watched("mp_play", resp, sizeof resp, 10);
    bcm_log_text("mp_play", r, resp);
    if (cfg_region_when & 4) bcm_region("after mp_play");

    if (v_live)
    {
        bcm_video_run(45);                 /* v13: really play for up to 45 s (stops early on PDS stop / end of stream) */
        BLOG("VIDEO RESULT: get_frame=%d frames_sent=%d bytes=%d empty=%d eos=%d stop=%d", v_getframes, v_frames, v_bytes, v_empty, v_eos, v_stop);
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

static bool dbg_bcm_host(void)
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

    bcm_log_fd = creat(BCM_LOG_PATH, 0666);
    bcm_t_start = current_tick;
    BLOG("bcm host test v24, built %s %s", __DATE__, __TIME__);
    bcm_load_cfg();
    bcm_apply_variant();

    fd = open(BCM_VMCS_PATH, O_RDONLY);
    if (fd < 0)
    {
        BLOG("cannot open %s", BCM_VMCS_PATH);
        splash(HZ*3, "vmcs.bin not found");
        goto out;
    }
    r = read(fd, bcm_vmcs_buf, sizeof bcm_vmcs_buf);
    close(fd);
    if (r <= 0 || r >= (int)sizeof bcm_vmcs_buf)
    {
        BLOG("vmcs.bin read failed (%d)", r);
        goto out;
    }
    BLOG("vmcs.bin %d bytes", r);

    splash(0, "Booting VideoCore from file...");
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
            if (bcm_log_fd >= 0) close(bcm_log_fd);
            bcm_log_fd = -1;
            system_reboot();
        }
    }
    splash(HZ*2, "v24 marker A: VC booted from file");   /* legacy LCD path: visible or black? */

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

    bcm_verify_upload((unsigned)(sizeof bcm_vmcs_buf < 201376 ? sizeof bcm_vmcs_buf : 201376));
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

    bcm_vfs_set_root(BCM_VC_ROOT);
    bcm_host_set_trace(bcm_trace_cb);
    for (i = 0; i < (int)(sizeof cmds / sizeof cmds[0]); i++)
    {
        bcm_cmd_t0 = current_tick;
        r = bcm_gencmd(cmds[i], resp, sizeof resp, 1000);
        bcm_log_text(cmds[i], r, resp);
    }

    {   /* listen for unsolicited traffic (VCFS/VCHR/VCPD requests) for 5 seconds */
        long end = current_tick + 5*HZ;
        while (TIME_BEFORE(current_tick, end))
        {
            bcm_host_service();
            sleep(HZ/100);
        }
        const struct bcm_host_stats *st = bcm_host_get_stats();
        BLOG("rx=%u tx=%u vcfs=%u pds=%u bad_magic=%u unknown=%u",
             (unsigned)st->rx_msgs, (unsigned)st->tx_msgs, (unsigned)st->vcfs_ops,
             (unsigned)st->pds_ops, (unsigned)st->bad_magic, (unsigned)st->unknown_ops);
        BLOG("truncated=%u resynced=%u", (unsigned)st->truncated, (unsigned)st->resynced);
    }

    bcm_step("vmcs + mplayer.vll loaded");
    bcm_pm_set();
    bcm_pm_info();
    bcm_step("pm_set_policy min / pm info");
    if (!cfg_skip_disp)
        bcm_display_probe();
    bcm_step("display_control probe");
    if (!bcm_check_dead())
        splash(HZ*2, "v24 marker B: after display_control");
    bcm_playback_probe();

    if (cfg_cmds_end && !bcm_check_dead() && !bcm_vc_stopped)
    {
        char big[1024]; int cr;
        bcm_cmd_t0 = current_tick;
        cr = bcm_gencmd_watched("commands", big, sizeof big, 6);
        bcm_log_text("commands (end of run: oversized-reply check)", cr, big);
        { const struct bcm_host_stats *st = bcm_host_get_stats();
          BLOG("end stats: rx=%u tx=%u truncated=%u resynced=%u bad_magic=%u", st->rx_msgs, st->tx_msgs, st->truncated, st->resynced, st->bad_magic); }
    }
    if (v_live && !bcm_check_dead())
        splashf(HZ*3, "v24: sent %d video frames", v_frames);
out:
    if (bcm_dead_seen || (cfg_reboot_end && bcm_log_fd >= 0))
    {
        /* The LCD code spins forever on a dead BCM, so do NOT touch it: save the log and reboot cleanly. */
        BLOG(bcm_dead_seen ? "VC dead -- log saved, rebooting the iPod" : "test finished -- log saved, rebooting the iPod (cfg reboot_end=0 to stay)");
        if (bcm_log_fd >= 0) close(bcm_log_fd);
        bcm_log_fd = -1;
        system_reboot();
    }
    if (bcm_log_fd >= 0)
        close(bcm_log_fd);
    bcm_log_fd = -1;
    splash(HZ*2, "log: " BCM_LOG_PATH);
    return false;
}
#endif /* IPOD_VIDEO */

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
