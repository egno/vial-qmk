/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Game mode state machine (OFF / GAME / CHAT). The modes are described in game_mode.h. */
#include QMK_KEYBOARD_H
#include "game_mode.h"
#include "hold_alone.h"
#include "trainer.h"

_Static_assert(GAME_LAYER_NAV < HOLD_ALONE_LAYER, "the game layers must stay below the hold-alone config layer");

#define GAME_BIT(layer) ((layer_state_t)1 << (layer))
#define GAME_SUB_LAYERS (GAME_BIT(GAME_LAYER_NUM) | GAME_BIT(GAME_LAYER_RIGHT) | GAME_BIT(GAME_LAYER_NAV))

/* Invariant: mode == GAME_MODE_GAME exactly while GAME_LAYER is on. It is
 * kept by game_mode_layer_state_set(), which sees every layer change, so the
 * mode also follows a layer 6 that was switched by something else (TG/TO). */
static game_mode_t mode = GAME_MODE_OFF;

/* The key that ended chat mode by position is still down; its release has to
 * be swallowed too (it arrives as the base-layer keycode from the source
 * layer cache, and its press was never handled). */
static bool     chat_return_held = false;
static keypos_t chat_return_key;

#if GAME_CHAT_IDLE_MS > 0
static uint32_t chat_timer = 0;
#endif

#ifdef COMBO_ENABLE
/* Combos match on keycode and hold keys back for the combo term, so they are
 * off while in GAME. combo_enable()/combo_disable() only flip a RAM flag in
 * process_combo.c (also in a Vial build), nothing is written to EEPROM.
 * combos_suspended: we switched them off and owe a combo_enable().
 * combo_sync_pending: the mode changed; applied from housekeeping, because
 * combo_disable() replays the combo key buffer through process_record and the
 * mode can change in the middle of process_record / layer_state_set. */
static bool combos_suspended   = false;
static bool combo_sync_pending = false;
#endif

game_mode_t game_mode_get(void) {
    return mode;
}

static void set_mode(game_mode_t new_mode) {
    if (new_mode == mode) return;
    mode = new_mode;
#ifdef COMBO_ENABLE
    combo_sync_pending = true;
#endif
#if GAME_CHAT_IDLE_MS > 0
    if (new_mode == GAME_MODE_CHAT) chat_timer = timer_read32();
#endif
}

/* OFF / CHAT -> GAME. The mode itself is set by game_mode_layer_state_set(),
 * which runs inside layer_on(): one place owns "layer 6 on means GAME". */
static void enter_game(void) {
    layer_on(GAME_LAYER);
}

/* GAME -> OFF / CHAT. Mods are deliberately left alone: keys still held
 * release through the source layer cache, clearing would drop a held Shift.
 *
 * The order matters, each of the two layer calls re-enters
 * game_mode_layer_state_set():
 *  1. unlock GNAV while the mode is still GAME and layer 6 still on, so that
 *     pass changes nothing but the lock;
 *  2. set the target mode, so that
 *  3. layer_off() finds the mode already settled: a CHAT stays CHAT instead
 *     of being read as "layer 6 was switched off from outside" -> OFF. */
static void leave_game(game_mode_t new_mode) {
#ifdef LAYER_LOCK_ENABLE
    layer_lock_off(GAME_LAYER_NAV);
#endif
    set_mode(new_mode);
    layer_off(GAME_LAYER);
}

/* True if a key other than `self` is down. The master's matrix holds both halves. */
static bool other_key_down(keyevent_t self) {
    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        matrix_row_t row = matrix_get_row(r);
        if (IS_KEYEVENT(self) && self.key.row == r) {
            row &= ~((matrix_row_t)1 << self.key.col);
        }
        if (row) return true;
    }
    return false;
}

/* A press that sends Enter or Esc: the plain key, or a tapped LT / MT. */
static bool is_chat_end_tap(uint16_t keycode, keyrecord_t *record) {
    if (IS_QK_LAYER_TAP(keycode)) {
        if (record->tap.count == 0) return false;
        keycode = QK_LAYER_TAP_GET_TAP_KEYCODE(keycode);
    } else if (IS_QK_MOD_TAP(keycode)) {
        if (record->tap.count == 0) return false;
        keycode = QK_MOD_TAP_GET_TAP_KEYCODE(keycode);
    }
    return keycode == KC_ENT || keycode == KC_PENT || keycode == KC_ESC;
}

bool game_mode_process_record(uint16_t keycode, keyrecord_t *record) {
#if GAME_CHAT_IDLE_MS > 0
    /* Every key event counts as chat activity. */
    if (mode == GAME_MODE_CHAT) chat_timer = timer_read32();
#endif

    if (chat_return_held && !record->event.pressed && IS_KEYEVENT(record->event) && KEYEQ(record->event.key, chat_return_key)) {
        chat_return_held = false;
        return false;
    }

    /* The trainer judges presses on the layers that are live, so it cannot be
     * started on top of GAME / CHAT: its toggle is inert until game mode is off. */
    if (keycode == TRAINER_TOGGLE && mode != GAME_MODE_OFF && !trainer_is_active()) return false;

    if (keycode == GAME_TOGGLE || keycode == GAME_CHAT) {
        /* Always consumed, press and release. Inert while drilling: the
         * trainer owns the layers and nothing may change under it. */
        if (!record->event.pressed || trainer_is_active()) return false;

        if (keycode == GAME_TOGGLE) {
            switch (mode) {
                case GAME_MODE_OFF:
                    enter_game();
                    break;
                case GAME_MODE_GAME:
                    leave_game(GAME_MODE_OFF);
                    break;
                case GAME_MODE_CHAT:
                    /* Layer 6 is already off. */
                    set_mode(GAME_MODE_OFF);
                    break;
            }
        } else if (mode == GAME_MODE_GAME) {
            /* GAME_CHAT sits next to a key that is used all the time. A slip
             * while other keys are held must not drop the player into the
             * base layer with its home-row mods. */
            if (!other_key_down(record->event)) leave_game(GAME_MODE_CHAT);
        } else if (mode == GAME_MODE_CHAT) {
            /* Only reachable if GAME_CHAT was also put on a typing layer. */
            enter_game();
        }
        return false;
    }

    /* In CHAT layer 6 is off, so the GAME_CHAT key arrives as whatever the
     * base layer has at its position: look it up on layer 6 instead. Only
     * with no other layer held, where a thumb key may mean something else. */
    if (mode == GAME_MODE_CHAT && record->event.pressed && IS_KEYEVENT(record->event) && layer_state == 0 && !trainer_is_active() && keymap_key_to_keycode(GAME_LAYER, record->event.key) == GAME_CHAT) {
        chat_return_held = true;
        chat_return_key  = record->event.key;
        enter_game();
        return false;
    }

    return true;
}

/* A tapped Enter / Esc ends chat mode. It has to happen here, on the press:
 *  - in process_record_user the key's action is resolved only afterwards, so
 *    Enter would already be looked up on layer 6;
 *  - on the release a key rolled over Enter has already been resolved on the
 *    base layer (and e.g. holds GUI in the game).
 * The release still finds its own action through the source layer cache. */
void game_mode_post_process_record(uint16_t keycode, keyrecord_t *record) {
    if (mode != GAME_MODE_CHAT || !record->event.pressed) return;
    if (is_chat_end_tap(keycode, record)) enter_game();
}

void game_mode_housekeeping(void) {
#ifdef COMBO_ENABLE
    if (combo_sync_pending) {
        combo_sync_pending = false;
        if (mode == GAME_MODE_GAME) {
            /* Only if they were on: a user who switched combos off keeps them off afterwards. */
            if (!combos_suspended && is_combo_enabled()) {
                combos_suspended = true;
                combo_disable();
            }
        } else if (combos_suspended) {
            combos_suspended = false;
            combo_enable();
        }
    }
#endif

#if GAME_CHAT_IDLE_MS > 0
    if (mode == GAME_MODE_CHAT) {
        if (trainer_is_active()) {
            /* The trainer consumes the keys and uses the layers; do not switch under it. */
            chat_timer = timer_read32();
        } else if (timer_elapsed32(chat_timer) >= GAME_CHAT_IDLE_MS) {
            enter_game();
        }
    }
#endif
}

/* Runs inside layer_state_set(): only edit `state` and the mode here, never
 * call layer_on / layer_off / layer_lock_off (they would recurse into this). */
layer_state_t game_mode_layer_state_set(layer_state_t state) {
    if (state & GAME_BIT(GAME_LAYER)) {
        set_mode(GAME_MODE_GAME);
        /* Both left thumbs give GNAV. Not while it is locked, or releasing a
         * thumb would switch the locked layer off. The lock only counts while
         * layer 9 is actually on: layer_lock_invert() switches the layer off
         * before it clears the lock bit, and a lock can be stale for one key
         * event after something else switched the layer off. */
        bool nav_locked = false;
#ifdef LAYER_LOCK_ENABLE
        nav_locked = is_layer_locked(GAME_LAYER_NAV) && (state & GAME_BIT(GAME_LAYER_NAV));
#endif
        if (!nav_locked) {
            state = update_tri_layer_state(state, GAME_LAYER_NUM, GAME_LAYER_RIGHT, GAME_LAYER_NAV);
        }
    } else {
        /* The helper layers exist only on top of layer 6. */
        state &= ~GAME_SUB_LAYERS;
        /* Layer 6 went off without leave_game() (TG / TO / layer_clear). CHAT stays CHAT. */
        if (mode == GAME_MODE_GAME) set_mode(GAME_MODE_OFF);
    }
    return state;
}
