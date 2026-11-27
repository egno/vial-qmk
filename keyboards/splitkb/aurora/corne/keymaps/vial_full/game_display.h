/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the keyboard shows for game mode (game_mode.c): per-key RGB cues on both halves,
 * the "CHAT" OLED label and the mouse-jiggler inhibit.
 */
#pragma once

#include "quantum.h"
#include "game_mode.h"

/* One bit per matrix position (row * MATRIX_COLS + col), both halves. */
#define GAME_DISPLAY_MASK_BYTES ((MATRIX_ROWS * MATRIX_COLS + 7) / 8)

/* Carried to the secondary half inside the status RPC (oled_status.c). Bytes only: no padding. */
typedef struct {
    uint8_t flags;                           /* bits 0-1: game_mode_t, bit 2: GAME_LAYER_NAV locked */
    uint8_t wasd[GAME_DISPLAY_MASK_BYTES];   /* keys to light while in GAME */
    uint8_t arrows[GAME_DISPLAY_MASK_BYTES]; /* keys to light while GAME_LAYER_NAV is locked */
} game_display_sync_t;

/* Master, every housekeeping pass (from status_housekeeping): refresh the state and copy it out. */
void game_display_collect(game_display_sync_t *out);
/* Secondary: take over what the master sent. */
void game_display_receive(const game_display_sync_t *in);

/* Valid on both halves (the secondary has no layer_state and no mode of its own). */
game_mode_t game_display_mode(void);
bool        game_display_nav_locked(void);

#ifdef RGB_MATRIX_ENABLE
/* Chained from rgb_matrix_indicators_advanced_user (trainer.c) while the trainer is off. */
bool game_rgb_indicators(uint8_t led_min, uint8_t led_max);
#endif

/* Keyboard-level hooks overridden in game_display.c (weak defaults in corne.c / mouse_jiggler.c). */
bool corne_chat_mode_user(void);
