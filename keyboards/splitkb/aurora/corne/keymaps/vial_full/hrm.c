/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Home-row mod tuning: Chordal Hold and Flow Tap.
 *
 * Both features are always compiled into a Vial build (builddefs/build_vial.mk
 * passes -DCHORDAL_HOLD -DFLOW_TAP_TERM=321), but they are gated at runtime by
 * Vial's QMK Settings, which default to off:
 *   QSID 26  "Chordal Hold"   bit 3 of QS.tapping_v2, reset value 0
 *   QSID 27  "Flow Tap term"  QS.flow_tap_term,       reset value 0 (disabled)
 * quantum/qmk_settings.c owns get_chordal_hold() and get_flow_tap_term() (strong
 * definitions), so a keymap cannot hook those, and the FLOW_TAP_TERM define is
 * never used as a timeout. What a keymap can do:
 *   - turn the two settings on in RAM after they were loaded from EEPROM
 *     (hrm_post_init), which works the same on a fresh and on a used board and
 *     writes nothing by itself;
 *   - say which hand each key belongs to (chordal_hold_layout, weak in the
 *     generated keyboard.c);
 *   - say which keys Flow Tap applies to (is_flow_tap_key, weak in
 *     action_tapping.c).
 *
 * Tapping term, Permissive Hold and Hold On Other Key Press stay whatever Vial
 * has stored; Chordal Hold only decides the same-hand case and leaves the
 * opposite-hand case to those.
 */
#include QMK_KEYBOARD_H
#include "hrm.h"
#ifdef QMK_SETTINGS
#    include "qmk_settings.h"
#endif

void hrm_post_init(void) {
#ifdef QMK_SETTINGS
    /* RAM only. Vial shows these values; they reach EEPROM only if any QMK
     * Setting is saved from Vial afterwards (it stores the whole block). */
#    ifdef HRM_CHORDAL_HOLD
    /* Unticking it in Vial lasts until the next power-up. */
    QS.tapping_v2 |= 1 << QS_tapping_chordal_hold_bit;
#    endif
#    if defined(HRM_FLOW_TAP_TERM) && HRM_FLOW_TAP_TERM > 0
    /* 0 is "never set" as well as "off", so a non-zero term saved from Vial wins. */
    if (QS.flow_tap_term == 0) {
        QS.flow_tap_term = HRM_FLOW_TAP_TERM;
    }
#    endif
#endif
}

#ifdef CHORDAL_HOLD
/*
 * The handedness generated from keyboard.json puts the thumbs on 'L'/'R', which
 * would turn "thumb layer-tap + key on the same half" into a tap. '*' exempts a
 * key from the opposite-hands rule, both as the tap-hold key and as the other key.
 *
 * The left outer home key (RGUI_T(KC_TAB)) is exempt too: it is not typed in
 * rolls, and it keeps one-handed GUI shortcuts (GUI+C/V/W/Tab...) instant.
 */
const char chordal_hold_layout[MATRIX_ROWS][MATRIX_COLS] PROGMEM = LAYOUT_split_3x6_3(
    'L', 'L', 'L', 'L', 'L', 'L',      'R', 'R', 'R', 'R', 'R', 'R',
    '*', 'L', 'L', 'L', 'L', 'L',      'R', 'R', 'R', 'R', 'R', 'R',
    'L', 'L', 'L', 'L', 'L', 'L',      'R', 'R', 'R', 'R', 'R', 'R',
                        '*', '*', '*', '*', '*', '*'
);
#endif

#ifdef FLOW_TAP_TERM
/*
 * Called for the tap-hold key and for the key pressed before it; Flow Tap acts
 * only when both return true. Same as the stock version minus Space and with
 * layer-taps excluded, so:
 *   - the thumb layer-taps are never forced to tap (their holds stay instant);
 *   - a mod-tap right after Space/Enter/Backspace/Tab/Esc is never forced to
 *     tap either (Shift for a capital at the start of a word keeps working).
 * Mod-taps whose tap is not a letter (Tab, brackets, F-keys, digits on the
 * other layers) are left alone as well.
 */
bool hrm_flow_tap_keycode(uint16_t keycode) {
    if (IS_QK_LAYER_TAP(keycode)) {
        return false;
    }
    switch (get_tap_keycode(keycode)) {
        case KC_A ... KC_Z:
        case KC_DOT:
        case KC_COMM:
        case KC_SCLN:
        case KC_SLSH:
            return true;
    }
    return false;
}

bool is_flow_tap_key(uint16_t keycode) {
    if ((get_mods() & (MOD_MASK_CG | MOD_BIT_LALT)) != 0) {
        return false; /* hotkeys */
    }
    return hrm_flow_tap_keycode(keycode);
}
#else
bool hrm_flow_tap_keycode(uint16_t keycode) {
    return false;
}
#endif

/* ------------------------------------------------------------------------ */
/* Read-only view for the host (host_hid.c, 0xF6). These read the same      */
/* table, function and settings the tapping engine uses.                    */
/* ------------------------------------------------------------------------ */

bool hrm_chordal_hold_enabled(void) {
#if !defined(CHORDAL_HOLD)
    return false;
#elif defined(QMK_SETTINGS)
    return QS_tapping_chordal_hold != 0; /* what get_chordal_hold() in qmk_settings.c tests */
#else
    return true;
#endif
}

char hrm_chordal_hand(keypos_t pos) {
#ifdef CHORDAL_HOLD
    return chordal_hold_handedness(pos); /* reads chordal_hold_layout above */
#else
    return '*';
#endif
}

uint16_t hrm_flow_tap_term(void) {
#if !defined(FLOW_TAP_TERM)
    return 0;
#else
#    if defined(QMK_SETTINGS)
    uint16_t term = QS.flow_tap_term; /* what get_flow_tap_term() in qmk_settings.c returns */
#    else
    uint16_t term = FLOW_TAP_TERM;
#    endif
    return term > 500 ? 500 : term; /* action_tapping.c clamps it the same way */
#endif
}
