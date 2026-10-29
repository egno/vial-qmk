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

/* Last Vial layer; not reachable from the keymap, used only as config. */
#ifndef HOLD_ALONE_LAYER
#    define HOLD_ALONE_LAYER (DYNAMIC_KEYMAP_LAYER_COUNT - 1)
#endif

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

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (!record->event.pressed) {
        return true;
    }

    /* Any press cancels; a held tap-hold key then re-arms for itself. */
    hold_alone_pending = false;
    if (record->event.type == KEY_EVENT && (IS_QK_LAYER_TAP(keycode) || IS_QK_MOD_TAP(keycode)) && record->tap.count == 0) {
        hold_alone_pending = true;
        hold_alone_key     = record->event.key;
        hold_alone_timer   = record->event.time;
    }
    return true;
}

/* Runs after the release is handled, so mods / layers are already off. */
void post_process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed || !hold_alone_pending || !KEYEQ(record->event.key, hold_alone_key)) {
        return;
    }
    hold_alone_pending = false;
    if (timer_elapsed(hold_alone_timer) < HOLD_ALONE_MAX_MS) {
        hold_alone_send(keymap_key_to_keycode(HOLD_ALONE_LAYER, record->event.key));
    }
}
