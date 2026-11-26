/* Hold-alone actions, configured in Vial.
 *
 * When a tap-hold key (layer-tap or mod-tap) is held past the tapping term
 * and released without any other key pressed, the keycode at the same
 * position on HOLD_ALONE_LAYER is tapped. Edit that layer in Vial to
 * assign or change actions; transparent / KC_NO means "do nothing".
 *
 * Supported actions: basic keys, modified keys (e.g. LGUI(KC_F12)),
 * and Vial macros (M0, M1, ...).
 */
#include QMK_KEYBOARD_H
#include "dynamic_keymap.h"
#include "hold_alone.h"

/* Longer holds are treated as "changed my mind" and send nothing. */
#ifndef HOLD_ALONE_MAX_MS
#    define HOLD_ALONE_MAX_MS 1000
#endif

static bool     hold_alone_pending = false;
static keypos_t hold_alone_key;
static uint16_t hold_alone_timer;

static void hold_alone_send(uint16_t keycode) {
    if (keycode > KC_TRNS && keycode <= QK_MODS_MAX) {
        tap_code16(keycode);
    } else if (IS_QK_MACRO(keycode)) {
        dynamic_keymap_macro_send(keycode - QK_MACRO);
    }
}

/* Called from user_hooks.c for every event. */
void hold_alone_process_record(uint16_t keycode, keyrecord_t *record) {
    if (!record->event.pressed) {
        return;
    }

    /* Any press cancels; a held tap-hold key then re-arms for itself. */
    hold_alone_pending = false;
    if (record->event.type == KEY_EVENT && (IS_QK_LAYER_TAP(keycode) || IS_QK_MOD_TAP(keycode)) && record->tap.count == 0) {
        hold_alone_pending = true;
        hold_alone_key     = record->event.key;
        hold_alone_timer   = record->event.time;
    }
}

/* Runs after the release is handled, so mods / layers are already off. */
void hold_alone_post_process_record(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed || !hold_alone_pending || !KEYEQ(record->event.key, hold_alone_key)) {
        return;
    }
    hold_alone_pending = false;
    if (timer_elapsed(hold_alone_timer) < HOLD_ALONE_MAX_MS) {
        hold_alone_send(keymap_key_to_keycode(HOLD_ALONE_LAYER, record->event.key));
    }
}
