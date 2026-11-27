/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Home-row mod tuning (Chordal Hold + Flow Tap); hooks are dispatched from user_hooks.c. */
#pragma once

#include "quantum.h"

/* Applies the keymap's tap-hold defaults (config.h) on top of the Vial QMK Settings in RAM. */
void hrm_post_init(void);

/* Read-only view of what hrm.c and the tapping engine decide with, for host_hid.c (0xF6). */

/* Chordal Hold is switched on (Vial QMK Setting, see hrm.c). */
bool hrm_chordal_hold_enabled(void);
/* Handedness of a matrix position as Chordal Hold sees it: 'L', 'R' or '*' (exempt). */
char hrm_chordal_hand(keypos_t pos);
/* Flow Tap term in ms as the tapping engine applies it; 0 = off. */
uint16_t hrm_flow_tap_term(void);
/* The keycode half of is_flow_tap_key(): true if `keycode` takes part in Flow Tap. */
bool hrm_flow_tap_keycode(uint16_t keycode);
