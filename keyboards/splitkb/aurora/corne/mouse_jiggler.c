/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mouse_jiggler.h"
#include "quantum.h"

/* Off by default: periodic USB mouse reports can line up with OLED power-save / I2C timing on
 * some builds; use QK_KB_0 to enable when you want jiggle. */
static bool     mouse_jiggler_feature_enabled = false;
static uint32_t mouse_jiggler_last_send_ms  = 0;
static int8_t   mouse_jiggler_step          = 1;

void mouse_jiggler_post_init_kb(void) {
    /* QMK leaves last_*_activity timestamps at 0 until the first key/encoder event.
     * last_input_activity_elapsed() from 0 is then huge and would jiggle immediately without a guard,
     * so the jiggler used to skip forever while the timestamp stayed 0. Seed "last activity" to
     * post-init time so idle is measured from boot/init until the first real input. */
#    ifdef SPLIT_KEYBOARD
    if (is_keyboard_master())
#    endif
    {
        uint32_t now = sync_timer_read32();
        set_activity_timestamps(now, now, now);
    }
}

void mouse_jiggler_housekeeping_kb(void) {
#    ifdef SPLIT_KEYBOARD
    if (!is_keyboard_master()) {
        return;
    }
#    endif
    if (!mouse_jiggler_feature_enabled) {
        return;
    }
    if (last_input_activity_elapsed() < (uint32_t)MOUSE_JIGGLER_IDLE_MS) {
        return;
    }
    uint32_t now = sync_timer_read32();
    if (mouse_jiggler_last_send_ms != 0 && sync_timer_elapsed32(mouse_jiggler_last_send_ms) < (uint32_t)MOUSE_JIGGLER_INTERVAL_MS) {
        return;
    }
    const mouse_xy_report_t delta = (mouse_xy_report_t)(mouse_jiggler_step * 2);
    report_mouse_t        report  = {0};
    report.x                      = delta;
    host_mouse_send(&report);
    report = (report_mouse_t){0};
    host_mouse_send(&report);
    mouse_jiggler_step = (mouse_jiggler_step > 0) ? -1 : 1;
    mouse_jiggler_last_send_ms = now;
}

bool mouse_jiggler_process_record_kb(uint16_t keycode, keyrecord_t *record) {
    if (keycode == QK_KB_0 && record->event.pressed) {
        mouse_jiggler_feature_enabled = !mouse_jiggler_feature_enabled;
        if (!mouse_jiggler_feature_enabled) {
            mouse_jiggler_last_send_ms = 0;
        }
        return false;
    }
    return true;
}

bool mouse_jiggler_is_feature_enabled(void) {
    return mouse_jiggler_feature_enabled;
}
