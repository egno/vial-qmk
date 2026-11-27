/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Game mode for the vial_full keymap: a plain QWERTY layer without tap-holds
 * or combos, plus a "chat" detour back to the normal typing layers.
 *
 *   OFF   normal keyboard, GAME_LAYER off.
 *   GAME  GAME_LAYER on. Its two left thumbs hold GAME_LAYER_NUM and
 *         GAME_LAYER_RIGHT; both together give GAME_LAYER_NAV, which can be
 *         locked with QK_LLCK. Combos are suspended.
 *   CHAT  GAME_LAYER off again so a chat line can be typed on the base layer
 *         with all its tap-holds. A tapped Enter / Esc, the key at GAME_CHAT's
 *         position, or GAME_CHAT_IDLE_MS without a key go back to GAME.
 *
 * Everything is RAM only: the keyboard always powers up in OFF.
 * Hooks are dispatched from user_hooks.c.
 */
#pragma once

#include "quantum.h"

#define GAME_LAYER 6
#define GAME_LAYER_NUM 7
#define GAME_LAYER_RIGHT 8
#define GAME_LAYER_NAV 9

/* Custom keycodes. Vial's customKeycodes[i] maps to QK_KB_i; 0 is the mouse jiggler, 1 the trainer. */
#define GAME_TOGGLE QK_KB_2
#define GAME_CHAT QK_KB_3

/* Chat mode falls back to the game layer after this long without a key event (ms); 0 = never. */
#ifndef GAME_CHAT_IDLE_MS
#    define GAME_CHAT_IDLE_MS 30000
#endif

typedef enum { GAME_MODE_OFF, GAME_MODE_GAME, GAME_MODE_CHAT } game_mode_t;

/* Master half only: the secondary has no layer state, so it always reads GAME_MODE_OFF here. */
game_mode_t game_mode_get(void);

/* Called from user_hooks.c. game_mode_process_record returns false when it consumed the event. */
bool          game_mode_process_record(uint16_t keycode, keyrecord_t *record);
void          game_mode_post_process_record(uint16_t keycode, keyrecord_t *record);
void          game_mode_housekeeping(void);
layer_state_t game_mode_layer_state_set(layer_state_t state);
