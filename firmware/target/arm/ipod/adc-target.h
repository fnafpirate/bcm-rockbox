/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
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
#ifndef _ADC_TARGET_H_
#define _ADC_TARGET_H_

#define ADC_BATTERY 0
#define ADC_ACCESSORY 1
#define ADC_UNREG_POWER ADC_BATTERY
#if defined(IPOD_VIDEO) || defined(IPOD_NANO)
#define ADC_4066_ISTAT 2
/* bcm overlay v59: the battery input in the PCF5060x's "subtractor" mode (ADCC2 mux 3) -- what Apple's firmware reads
   (rt 0x265f74 -> 0x28341c(3, ...)): about twice the resolution of the resistive-divider mode Rockbox uses */
#define ADC_BATTERY_SUBTR 3
#define NUM_ADC_CHANNELS 4
#else
#define NUM_ADC_CHANNELS 2
#endif
 
/* Force a scan now */
unsigned short adc_scan(int channel);
#endif
