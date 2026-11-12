/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Alan Korr & Nick Robinson
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* Lingo 0x02, Simple Remote Lingo
 *
 * TODO:
 * - Fix cmd 0x00 handling, there has to be a more elegant way of doing
 *   this
 */

#include "iap-core.h"
#include "misc.h"
#include "iap-lingo.h"
#include "system.h"
#include "button.h"
#include "audio.h"
#include "settings.h"
#include "tuner.h"
#if CONFIG_TUNER
#include "ipod_remote_tuner.h"
#endif
#ifdef HAVE_BCM_VIDEOCORE
#include "bcm_host.h"
#endif

/*
 * This macro is meant to be used inside an IAP mode message handler.
 * It is passed the expected minimum length of the message buffer.
 * If the buffer does not have the required lenght an ACK
 * packet with a Bad Parameter error is generated.
 */
#define CHECKLEN(x) do { \
        if (len < (x)) { \
            cmd_ack(cmd, IAP_ACK_BAD_PARAM); \
            return; \
        }} while(0)

static void cmd_ack(const unsigned char cmd, const unsigned char status)
{
    IAP_TX_INIT(0x02, 0x01);
    IAP_TX_PUT(status);
    IAP_TX_PUT(cmd);

    iap_send_tx();
}

#define cmd_ok(cmd) cmd_ack((cmd), IAP_ACK_OK)

void iap_handlepkt_mode2(const unsigned int len, const unsigned char *buf)
{
    static bool poweron_pressed = false;
#if CONFIG_TUNER
    static bool remote_mute = false;
#endif
    unsigned int cmd = buf[1];

    /* We expect at least three bytes in the buffer, one for the
     * lingo, one for the command, and one for the first button
     * state bits.
     */
    CHECKLEN(3);

    /* Lingo 0x02 must have been negotiated, except for
     * ContextButtonStatus (0x00): simple remotes like the Apple A1018
     * identify only once at power-up. If the remote was already
     * powered before Rockbox started (e.g. plugged in at boot) that
     * identification is never seen, and rejecting the button events
     * would leave the remote dead until it is replugged. The OF
     * accepts these events without identification, so do the same.
     */
    if ((cmd != 0x00) && !DEVICE_LINGO_SUPPORTED(0x02)) {
        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
        return;
    }

    switch (cmd)
    {
        /* ContextButtonStatus (0x00)
         *
         * Transmit button events from the device to the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Simple Remote Lingo, always 0x02
         * 0x01: Command, always 0x00
         * 0x02: Button states 0:7
         * 0x03: Button states 8:15 (optional)
         * 0x04: Button states 16:23 (optional)
         * 0x05: Button states 24:31 (optional)
         *
         * Returns: (none)
         */
        case 0x00:
        {
            unsigned long btn = BUTTON_NONE;
#ifdef IPOD_VIDEO
            /* v63: while a video plays, the ContextButtonStatus bits that are commands rather than buttons go to the
             * video player (the button bits -- play/pause, volume, >>| |<<, FF/REW, menu, select, up/down -- reach it
             * as BUTTON_RC_* like always).  Bit numbers as in the iAP spec: 7 stop, 8 play, 9 pause, 10 mute,
             * 11/12 next/previous chapter, 18 power off. */
            unsigned char vbuf[6];
            static uint32_t vprev;
            /* v74: the remote's volume works everywhere (menus, browser, Now Playing), not only where the keymap has
               it: a press = one step, held = a step every 0.1 s after 0.4 s */
            static long vnext;
            static int vdir;
            if (!IAP_VIDEO_ACTIVE() && len >= 3 && (buf[2] & 6))
            {
                int d = (buf[2] & 2) ? 1 : -1;
                if (d != vdir || TIME_AFTER(current_tick, vnext))
                {
                    adjust_volume(d);
                    vnext = current_tick + (d != vdir ? HZ * 4 / 10 : HZ / 10);
                    vdir = d;
                }
            }
            else
                vdir = 0;
            if (IAP_VIDEO_ACTIVE())
            {
                uint32_t bits = 0, pressed;
                unsigned int i;
                for (i = 2; i < len && i < 6; i++)
                    bits |= (uint32_t)buf[i] << (8 * (i - 2));
                pressed = bits & ~vprev;
                vprev = bits;
                if (pressed & BIT_N(7))  bcm_video_remote(IAPV_STOP, 0);
                if (pressed & BIT_N(8))  bcm_video_remote(IAPV_PLAY, 0);
                if (pressed & BIT_N(9))  bcm_video_remote(IAPV_PAUSE, 0);
                if (pressed & BIT_N(10)) bcm_video_remote(IAPV_MUTE, 0);
                if (pressed & BIT_N(11)) bcm_video_remote(IAPV_CH_NEXT, 0);
                if (pressed & BIT_N(12)) bcm_video_remote(IAPV_CH_PREV, 0);
                if (pressed & BIT_N(18)) bcm_video_remote(IAPV_STOP, 0);
                bits &= ~(BIT_N(7) | BIT_N(8) | BIT_N(9) | BIT_N(10) | BIT_N(11) | BIT_N(12) | BIT_N(18));
                memset(vbuf, 0, sizeof vbuf);
                vbuf[0] = buf[0]; vbuf[1] = buf[1];
                for (i = 2; i < len && i < 6; i++)
                    vbuf[i] = (bits >> (8 * (i - 2))) & 0xff;
                buf = vbuf;
            }
            else
                vprev = 0;
#endif

            if(buf[2] != 0)
            {
                if(buf[2] & 1)
                {
                    btn |= BUTTON_RC_PLAY;
#if CONFIG_TUNER
                    if (radio_present == 1) {
                        if (remote_mute == 0) {
                            /* Not Muted so  radio on*/
                            tuner_set(RADIO_MUTE,0);
                        } else {
                            /* Muted so  radio off*/
                            tuner_set(RADIO_MUTE,1);
                        }
                        remote_mute = !remote_mute;
                    }
#endif
                }
#ifdef IPOD_VIDEO
                if(IAP_VIDEO_ACTIVE())          /* v74: else handled above */
#endif
                {
                if(buf[2] & 2)
                    btn |= BUTTON_RC_VOL_UP;
                if(buf[2] & 4)
                    btn |= BUTTON_RC_VOL_DOWN;
                }
                if(buf[2] & 8)
                    btn |= BUTTON_RC_RIGHT;
                if(buf[2] & 16)
                    btn |= BUTTON_RC_LEFT;
            }
            else if(len >= 4 && buf[3] != 0)
            {
                if(buf[3] & 1) /* play */
                {
                    if (audio_status() != AUDIO_STATUS_PLAY)
                        btn |= BUTTON_RC_PLAY;
#if CONFIG_TUNER
                    if (radio_present == 1) {
                        tuner_set(RADIO_MUTE,0);
                    }
#endif
                }
                if(buf[3] & 2) /* pause */
                {
                    if (audio_status() == AUDIO_STATUS_PLAY)
                        btn |= BUTTON_RC_PLAY;
#if CONFIG_TUNER
                    if (radio_present == 1) {
                        tuner_set(RADIO_MUTE,1);
                    }
#endif
                }
                if(buf[3] & 128) /* Shuffle */
                {
                    if (!iap_btnshuffle)
                    {
                        iap_shuffle_state(!global_settings.playlist_shuffle);
                        iap_btnshuffle = true;
                    }
                }
            }
            else if(len >= 5 && buf[4] != 0)
            {
                if(buf[4] & 1) /* repeat */
                {
                    if (!iap_btnrepeat)
                    {
                        iap_repeat_next();
                        iap_btnrepeat = true;
                    }
                }

                if (buf[4] & 2) /* power on */
                {
                    poweron_pressed = true;
                }

                /* Power off
                 * Not quite sure how to react to this, but stopping playback
                 * is a good start.
                 */
                if (buf[4] & 0x04)
                {
                    if (audio_status() == AUDIO_STATUS_PLAY)
                        btn |= BUTTON_RC_PLAY;
                }

                if(buf[4] & 16) /* ffwd */
                    btn |= BUTTON_RC_RIGHT;
                if(buf[4] & 32) /* frwd */
                    btn |= BUTTON_RC_LEFT;
                if(buf[4] & 64) /* menu */
                    btn |= BUTTON_RC_MENU;
                if(buf[4] & 128) /* select */
                    btn |= BUTTON_RC_SELECT;
            }
            else if(len >= 6 && buf[5] != 0)
            {
                if(buf[5] & 1) /* up */
                    btn |= BUTTON_RC_UP;
                if (buf[5] & 2) /* down */
                    btn |= BUTTON_RC_DOWN;
            }

#ifdef HAVE_BCM_VIDEOCORE
            /* When BCM video is playing, route remote buttons to video
             * player: play/pause toggles stop, ffwd/frwd seek by
             * bcm_video_skip_seconds, menu stops playback.
             */
            if (bcm_is_active())
            {
                if (btn & BUTTON_RC_PLAY)
                    bcm_video_quiz_stop();
                if (btn & BUTTON_RC_RIGHT)
                {
                    int pos = bcm_video_quiz_get_pos_ms();
                    int skip_ms = global_settings.bcm_video_skip_seconds * 1000;
                    bcm_video_quiz_start(NULL, pos + skip_ms);
                }
                if (btn & BUTTON_RC_LEFT)
                {
                    int pos = bcm_video_quiz_get_pos_ms();
                    int skip_ms = global_settings.bcm_video_skip_seconds * 1000;
                    int newpos = pos - skip_ms;
                    bcm_video_quiz_start(NULL, (newpos < 0) ? 0 : newpos);
                }
                btn = BUTTON_NONE; /* consumed */
            }
#endif /* HAVE_BCM_VIDEOCORE */

            /* Commit the new button state in one go. The button driver
             * reads iap_remotebtn from interrupt context, a transient
             * BUTTON_NONE between two repeated button-down events would
             * register as a spurious release/press pair.
             */
            if (btn != BUTTON_NONE)
            {
                /* Only re-arm iap_repeatbtn when the state changes. A
                 * held button repeats its down event every 30-100ms,
                 * re-arming on every repeat makes iap_handlepkt()
                 * delay each following packet, so packets back up
                 * during the hold and stale events replay as phantom
                 * keypresses after the release.
                 */
                if (btn != iap_remotebtn)
                    iap_repeatbtn = 2;
                iap_remotebtn = btn;
                iap_timeoutbtn = 3;
            }
            else
            {
                iap_remotebtn = BUTTON_NONE;
                iap_timeoutbtn = 0;
            }

            /* power on released */
            if (poweron_pressed && len >= 5 && !(buf[4] & 2))
            {
                poweron_pressed = false;
#ifdef HAVE_LINE_REC
                /* Belkin TuneTalk microphone sends power-on press+release
                 * events once authentication sequence is finished,
                 * GetDevCaps command is ignored by the device when it is
                 * sent before power-on release event is received.
                 * XXX: It is unknown if other microphone devices are
                 * sending the power-on events.
                 */
                if (DEVICE_LINGO_SUPPORTED(0x01)) {
                    /* GetDevCaps */
                    IAP_TX_INIT(0x01, 0x07);
                    iap_send_tx();
                }
#endif
            }

            break;
        }
        /* ACK (0x01)
         *
         * Sent from the iPod to the device
         */

        /* ImageButtonStatus (0x02)
         *
         * Transmit image button events from the device to the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Simple Remote Lingo, always 0x02
         * 0x01: Command, always 0x02
         * 0x02: Button states 0:7
         * 0x03: Button states 8:15 (optional)
         * 0x04: Button states 16:23 (optional)
         * 0x05: Button states 24:31 (optional)
         *
         * This command requires authentication
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_*
         */
        case 0x02:
        {
            if (!DEVICE_AUTHENTICATED) {
                cmd_ack(cmd, IAP_ACK_NO_AUTHEN);
                break;
            }

            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            break;
        }

        /* VideoButtonStatus (0x03)
         *
         * Transmit video button events from the device to the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Simple Remote Lingo, always 0x02
         * 0x01: Command, always 0x03
         * 0x02: Button states 0:7
         * 0x03: Button states 8:15 (optional)
         * 0x04: Button states 16:23 (optional)
         * 0x05: Button states 24:31 (optional)
         *
         * This command requires authentication
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_*
         */
        case 0x03:
        {
#ifdef IPOD_VIDEO
            iap_dock_video_interest();          /* v74: sends video buttons: a video dock */
            /* v63: video docks / AV remotes.  Bits: 0 play/pause, 1 next video, 2 previous video, 3 stop, 4 play,
             * 5 pause, 6 begin FF, 7 begin REW (released = end), 8 next chapter, 9 previous chapter,
             * 10/11 next/previous frame (not supported: ignored).  The 5G/5.5G took these without authentication. */
            static uint32_t vprev;
            uint32_t bits = 0, pressed, released;
            unsigned int i;
            if (!iap_ident_stock() && !DEVICE_AUTHENTICATED) {
                cmd_ack(cmd, IAP_ACK_NO_AUTHEN);
                break;
            }
            if (!IAP_VIDEO_ACTIVE()) {
                vprev = 0;
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);   /* no video playing */
                break;
            }
            for (i = 2; i < len && i < 6; i++)
                bits |= (uint32_t)buf[i] << (8 * (i - 2));
            pressed = bits & ~vprev;
            released = vprev & ~bits;
            vprev = bits;
            if (pressed & BIT_N(0)) bcm_video_remote(IAPV_TOGGLE, 0);
            if (pressed & BIT_N(1)) bcm_video_remote(IAPV_NEXT, 0);
            if (pressed & BIT_N(2)) bcm_video_remote(IAPV_PREV, 0);
            if (pressed & BIT_N(3)) bcm_video_remote(IAPV_STOP, 0);
            if (pressed & BIT_N(4)) bcm_video_remote(IAPV_PLAY, 0);
            if (pressed & BIT_N(5)) bcm_video_remote(IAPV_PAUSE, 0);
            if (pressed & BIT_N(6)) bcm_video_remote(IAPV_FF, 0);
            else if (pressed & BIT_N(7)) bcm_video_remote(IAPV_REW, 0);
            if ((released & (BIT_N(6) | BIT_N(7))) && !(bits & (BIT_N(6) | BIT_N(7))))
                bcm_video_remote(IAPV_FFREW_END, 0);
            if (pressed & BIT_N(8)) bcm_video_remote(IAPV_CH_NEXT, 0);
            if (pressed & BIT_N(9)) bcm_video_remote(IAPV_CH_PREV, 0);
            cmd_ok(cmd);
            break;
#else
            if (!DEVICE_AUTHENTICATED) {
                cmd_ack(cmd, IAP_ACK_NO_AUTHEN);
                break;
            }

            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            break;
#endif
        }

        /* AudioButtonStatus (0x04)
         *
         * Transmit audio button events from the device to the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Simple Remote Lingo, always 0x02
         * 0x01: Command, always 0x04
         * 0x02: Button states 0:7
         * 0x03: Button states 8:15 (optional)
         * 0x04: Button states 16:23 (optional)
         * 0x05: Button states 24:31 (optional)
         *
         * This command requires authentication
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_*
         */
        case 0x04:
        {
            unsigned char repeatbuf[6];

#ifdef IPOD_VIDEO
            if (!iap_ident_stock() && !DEVICE_AUTHENTICATED) {   /* v63: no authentication on the 5G/5.5G */
#else
            if (!DEVICE_AUTHENTICATED) {
#endif
                cmd_ack(cmd, IAP_ACK_NO_AUTHEN);
                break;
            }

            /* This is basically the same command as ContextButtonStatus (0x00),
             * with the difference that it requires authentication and that
             * it returns an ACK packet to the device.
             * So just route it through the handler again, with 0x00 as the
             * command
             */
            memcpy(repeatbuf, buf, 6);
            repeatbuf[1] = 0x00;
            iap_handlepkt_mode2((len<6)?len:6, repeatbuf);

            cmd_ok(cmd);
            break;
        }

        /* The default response is IAP_ACK_BAD_PARAM */
        default:
        {
#ifdef LOGF_ENABLE
            logf("iap: Unsupported Mode02 Command");
#endif
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
    }
}
