/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Passive mod-tap misfire log. The heuristics and their limits are described in misfire.h. */
#include QMK_KEYBOARD_H
#include "misfire.h"
#include "trainer.h"
#include <string.h>

_Static_assert(MISFIRE_MAX_KEYS <= 16, "tracked keys are kept in 16-bit masks");

#define MF_NONE 0xFF

static misfire_entry_t entries[MISFIRE_MAX_KEYS];
static uint8_t         entry_count = 0;

/* Transient state; one bit per entry in the masks. */
static struct {
    uint16_t holds_down; /* tracked mod-taps currently down as hold */
    uint16_t taps_down;  /* tracked mod-taps currently down as tap */
    uint16_t rolled;     /* of taps_down: another key was already pressed over it */
    uint8_t  last_tap;   /* most recent tracked tap press */

    uint8_t hold_key;    /* hold being watched, MF_NONE if none */
    uint8_t hold_others; /* other presses while it was down */
    bool    hold_alone;  /* no second tracked hold joined it */

    uint8_t  held_cand; /* hold released after exactly one other key; waiting for Backspace */
    uint32_t held_cand_time;

    uint8_t  tap_cand; /* tap that was rolled over; waiting for Backspace */
    uint8_t  tap_cand_presses;
    uint32_t tap_cand_time;
} st;

static void clear_transient(void) {
    memset(&st, 0, sizeof(st));
    st.last_tap = st.hold_key = st.held_cand = st.tap_cand = MF_NONE;
}

static int8_t find_entry(keypos_t pos) {
    for (uint8_t i = 0; i < entry_count; i++) {
        if (KEYEQ(entries[i].pos, pos)) return i;
    }
    return -1;
}

static bool base_is_mod_tap(keypos_t pos) {
    return IS_QK_MOD_TAP(keymap_key_to_keycode(0, pos));
}

static int8_t add_entry(keypos_t pos) {
    if (entry_count >= MISFIRE_MAX_KEYS) return -1;
    entries[entry_count] = (misfire_entry_t){.pos = pos};
    return entry_count++;
}

static void bump16(uint16_t *v) {
    if (*v < UINT16_MAX) (*v)++;
}

static void bump32(uint32_t *v) {
    if (*v < UINT32_MAX) (*v)++;
}

static bool is_backspace_tap(uint16_t keycode, keyrecord_t *record) {
    if (IS_QK_LAYER_TAP(keycode)) {
        return record->tap.count > 0 && QK_LAYER_TAP_GET_TAP_KEYCODE(keycode) == KC_BSPC;
    }
    if (IS_QK_MOD_TAP(keycode)) {
        return record->tap.count > 0 && QK_MOD_TAP_GET_TAP_KEYCODE(keycode) == KC_BSPC;
    }
    if (IS_QK_MODS(keycode)) {
        return QK_MODS_GET_BASIC_KEYCODE(keycode) == KC_BSPC;
    }
    return keycode == KC_BSPC;
}

void misfire_reset(void) {
    entry_count = 0;
    memset(entries, 0, sizeof(entries));
    clear_transient();
    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            keypos_t pos = {.row = r, .col = c};
            if (base_is_mod_tap(pos)) add_entry(pos);
        }
    }
}

void misfire_post_init(void) {
    misfire_reset();
}

uint8_t misfire_count(void) {
    return entry_count;
}

bool misfire_get(uint8_t index, misfire_entry_t *out) {
    if (index >= entry_count) return false;
    *out = entries[index];
    return true;
}

static void on_release(int8_t idx) {
    if (idx < 0) return;
    uint16_t bit = 1u << idx;
    if (st.holds_down & bit) {
        st.holds_down &= ~bit;
        if (st.hold_key == idx) {
            if (st.hold_alone && st.hold_others == 1) {
                st.held_cand      = idx;
                st.held_cand_time = timer_read32();
            }
            st.hold_key = MF_NONE;
        }
    }
    st.taps_down &= ~bit;
    st.rolled &= ~bit;
}

void misfire_process_record(uint16_t keycode, keyrecord_t *record) {
    if (trainer_is_active()) {
        /* Drill input says nothing about real typing; also drop half-seen sequences. */
        clear_transient();
        return;
    }
    if (!IS_KEYEVENT(record->event)) return; /* encoders, combos */

    keypos_t pos = record->event.key;
    int8_t   idx = find_entry(pos);

    if (!record->event.pressed) {
        on_release(idx);
        return;
    }

    bool mod_tap = IS_QK_MOD_TAP(keycode);
    if (mod_tap && idx < 0 && base_is_mod_tap(pos)) {
        idx = add_entry(pos); /* base layer was edited in Vial since the last scan */
    }
    bool tracked = mod_tap && idx >= 0;
    bool hold    = tracked && record->tap.count == 0;

    /* 1. Is this press the Backspace a pending candidate is waiting for? */
    bool bspc = is_backspace_tap(keycode, record);
    if (st.held_cand != MF_NONE) {
        if (bspc && timer_elapsed32(st.held_cand_time) < MISFIRE_WINDOW_MS) {
            bump16(&entries[st.held_cand].held_misfires);
        }
        st.held_cand = MF_NONE; /* only the very next press counts */
    }
    if (st.tap_cand != MF_NONE) {
        if (bspc) {
            if (timer_elapsed32(st.tap_cand_time) < MISFIRE_WINDOW_MS) {
                bump16(&entries[st.tap_cand].tapped_misfires);
            }
            st.tap_cand = MF_NONE;
        } else if (++st.tap_cand_presses > 1) {
            st.tap_cand = MF_NONE; /* one key in between is allowed, not two */
        }
    }

    /* 2. This press rolls over any tracked mod-tap that is down as a tap.
     *    Only the first key over each one counts, and Backspace is not a roll. */
    uint16_t fresh = st.taps_down & ~st.rolled;
    st.rolled |= fresh;
    if (fresh && !bspc) {
        uint8_t victim = (st.last_tap != MF_NONE && (fresh & (1u << st.last_tap))) ? st.last_tap : (uint8_t)__builtin_ctz(fresh);
        st.tap_cand         = victim;
        st.tap_cand_presses = 0;
        st.tap_cand_time    = timer_read32();
    }

    /* 3. Totals and the hold being watched. */
    if (hold) {
        bump32(&entries[idx].holds);
        if (st.holds_down == 0) {
            st.hold_key    = idx;
            st.hold_others = 0;
            st.hold_alone  = true;
        } else {
            st.hold_alone = false;
        }
        st.holds_down |= 1u << idx;
        return;
    }
    if (st.hold_key != MF_NONE && st.hold_others < UINT8_MAX) {
        st.hold_others++;
    }
    if (tracked) {
        bump32(&entries[idx].taps);
        st.taps_down |= 1u << idx;
        st.rolled &= ~(1u << idx);
        st.last_tap = idx;
    }
}
