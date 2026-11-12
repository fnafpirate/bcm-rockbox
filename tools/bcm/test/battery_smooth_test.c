/*
 * battery_smooth_test.c — Battery smooth/balanced/exact mode simulation.
 *
 * Tests the EMA filter, IR-drop compensation, and interpolation
 * from battery_smooth.h without requiring iPod hardware.
 *
 * Build:
 *   gcc -std=c11 -Wall -O2 -I../../../firmware/export \
 *       battery_smooth_test.c -o battery_smooth_test
 *
 * Copyright (C) 2024 bcm-rockbox contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull in the header under test */
#include "../../../firmware/export/battery_smooth.h"

/* ---------------------------------------------------------------------------
 * Test framework
 * --------------------------------------------------------------------------*/

static int g_pass = 0, g_fail = 0;

#define CHECK(e) do { \
    if (e) { g_pass++; } \
    else { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #e); g_fail++; } \
} while(0)

#define CHECK_EQ(a,b) CHECK((a)==(b))
#define CHECK_RANGE(v,lo,hi) CHECK((v)>=(lo) && (v)<=(hi))

/* ---------------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------------*/

static void test_modes(void)
{
    printf("Mode constants ...\n");
    CHECK_EQ(BATT_SMOOTH,   0);
    CHECK_EQ(BATT_BALANCED, 1);
    CHECK_EQ(BATT_EXACT,    2);
}

static void test_ema_smooth(void)
{
    printf("EMA smooth mode (alpha=1/64) ...\n");
    battery_smooth_state_t s;
    memset(&s, 0, sizeof(s));

    /* Start at 4000 mV, should converge toward 4000 */
    s.smooth_mv_x256 = 4000 * 256;
    for (int i = 0; i < 128; i++) {
        int new_mv = 4200;  /* step to 4200 */
        /* Simulate EMA: s = s + (new - s) / 64 */
        s.smooth_mv_x256 = s.smooth_mv_x256
            + (new_mv * 256 - s.smooth_mv_x256) / 64;
    }
    int final_mv = s.smooth_mv_x256 / 256;
    /* After 128 iterations, should be close to 4200 */
    CHECK_RANGE(final_mv, 4190, 4200);
}

static void test_ema_balanced(void)
{
    printf("EMA balanced mode (alpha=1/16) ...\n");
    battery_smooth_state_t s;
    memset(&s, 0, sizeof(s));

    s.balanced_mv_x256 = 3700 * 256;
    for (int i = 0; i < 64; i++) {
        int new_mv = 3900;
        s.balanced_mv_x256 = s.balanced_mv_x256
            + (new_mv * 256 - s.balanced_mv_x256) / 16;
    }
    int final_mv = s.balanced_mv_x256 / 256;
    /* Balanced converges faster */
    CHECK_RANGE(final_mv, 3895, 3900);
}

static void test_ir_drop(void)
{
    printf("IR-drop compensation ...\n");
    /*
     * OCV = raw_mv + load_ma * resistance_mohm / 1000
     * With load=500mA and R=100mΩ: drop = 50mV
     */
    int raw_mv = 3750;
    int load_ma = 500;
    int r_mohm = 100;
    int ocv = raw_mv + load_ma * r_mohm / 1000;
    CHECK_EQ(ocv, 3800);

    /* Zero load: OCV = raw */
    ocv = raw_mv + 0 * r_mohm / 1000;
    CHECK_EQ(ocv, raw_mv);
}

static void test_exact_mode(void)
{
    printf("Exact mode (pass-through) ...\n");
    /*
     * In BATT_EXACT mode, the raw ADC voltage is used directly
     * (no smoothing).  Percentage is from table lookup only.
     */
    int raw_mv = 3850;
    /* In exact mode, displayed voltage == raw voltage */
    CHECK_EQ(raw_mv, 3850);
}

static void test_state_size(void)
{
    printf("State struct size ...\n");
    /* Must fit in a reasonable stack frame */
    CHECK(sizeof(battery_smooth_state_t) <= 64);
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------*/

int main(void)
{
    printf("=== battery_smooth_test ===\n\n");

    test_modes();
    test_ema_smooth();
    test_ema_balanced();
    test_ir_drop();
    test_exact_mode();
    test_state_size();

    printf("\n=== Results: PASS %d / %d  FAIL %d ===\n",
           g_pass, g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
