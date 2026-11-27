/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Game mode feedback (the state machine itself is game_mode.c):
 *
 * - RGB, both halves: GAME lights the W/A/S/D keys, a locked GAME_LAYER_NAV lights its arrow
 *   keys in a second colour, CHAT turns every key LED red. The player does not look at the
 *   OLED, so "you are typing on the Colemak layer" has to be impossible to miss.
 * - OLED (master, corne.c): "CHAT" in place of the blank layer-0 label.
 * - Mouse jiggler: held off while a game is running; it would nudge the aim.
 *
 * The secondary half knows neither the mode nor the layers, and its own EEPROM keymap cannot
 * be trusted either: Vial writes only reach the master, so after a remap the two halves
 * disagree about where W/A/S/D are. The master therefore resolves the keys to matrix
 * positions and ships mode + positions inside the status RPC (oled_status.c); both halves
 * draw from that same struct.
 */
#include "game_display.h"
#include "mouse_jiggler.h"
#include <string.h>

#define GD_MODE_MASK 0x03
#define GD_NAV_LOCKED 0x04
_Static_assert(GAME_MODE_CHAT <= GD_MODE_MASK, "game_mode_t must fit the mode bits of the sync flags");

/* The key positions are looked up again this often while in game mode, so a Vial remap shows up. */
#define GD_RESCAN_MS 250

#ifndef GAME_RGB_WASD_HUE
#    define GAME_RGB_WASD_HUE 128 /* cyan */
#endif
#ifndef GAME_RGB_ARROWS_HUE
#    define GAME_RGB_ARROWS_HUE 213 /* magenta; red is taken by CHAT and the trainer */
#endif
#ifndef GAME_RGB_CHAT_MIN_VAL
#    define GAME_RGB_CHAT_MIN_VAL 48 /* brightness floor of the CHAT alarm, same as the trainer's */
#endif

/* What this half displays: built by game_display_collect on the master, received on the secondary. */
static game_display_sync_t gd;

static bool gd_is_wasd(uint16_t kc) {
    return kc == KC_W || kc == KC_A || kc == KC_S || kc == KC_D;
}

static bool gd_is_arrow(uint16_t kc) {
    return kc == KC_LEFT || kc == KC_DOWN || kc == KC_UP || kc == KC_RGHT;
}

static void gd_scan(uint8_t layer, bool (*match)(uint16_t), uint8_t *mask) {
    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            if (match(keymap_key_to_keycode(layer, (keypos_t){.row = r, .col = c}))) {
                uint8_t bit = r * MATRIX_COLS + c;
                mask[bit / 8] |= (uint8_t)(1u << (bit % 8));
            }
        }
    }
}

void game_display_collect(game_display_sync_t *out) {
    static uint32_t scanned;
    game_mode_t     mode  = game_mode_get();
    uint8_t         flags = (uint8_t)mode & GD_MODE_MASK;
#ifdef LAYER_LOCK_ENABLE
    /* Leaving GAME unlocks the layer, so a lock without GAME is a leftover, not a state to show. */
    if (mode == GAME_MODE_GAME && is_layer_locked(GAME_LAYER_NAV)) flags |= GD_NAV_LOCKED;
#endif
    if (flags != gd.flags || (flags && timer_elapsed32(scanned) >= GD_RESCAN_MS)) {
        game_display_sync_t now = {.flags = flags};
        if (mode == GAME_MODE_GAME) gd_scan(GAME_LAYER, gd_is_wasd, now.wasd);
        if (flags & GD_NAV_LOCKED) gd_scan(GAME_LAYER_NAV, gd_is_arrow, now.arrows);
        gd      = now;
        scanned = timer_read32();
    }
    *out = gd;
}

void game_display_receive(const game_display_sync_t *in) {
    gd = *in;
}

game_mode_t game_display_mode(void) {
    return is_keyboard_master() ? game_mode_get() : (game_mode_t)(gd.flags & GD_MODE_MASK);
}

bool game_display_nav_locked(void) {
#ifdef LAYER_LOCK_ENABLE
    if (is_keyboard_master()) return is_layer_locked(GAME_LAYER_NAV);
#endif
    return gd.flags & GD_NAV_LOCKED;
}

#ifdef RGB_MATRIX_ENABLE
/* Each half is only handed its own LED range; RGB_MATRIX_INDICATOR_SET_COLOR drops the rest. */
static void gd_light(const uint8_t *mask, uint8_t hue, uint8_t val, uint8_t led_min, uint8_t led_max) {
    rgb_t rgb = hsv_to_rgb((hsv_t){hue, 255, val});
    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            uint8_t bit = r * MATRIX_COLS + c;
            if (!(mask[bit / 8] & (1u << (bit % 8)))) continue;
            uint8_t idx = g_led_config.matrix_co[r][c];
            if (idx == NO_LED) continue;
            RGB_MATRIX_INDICATOR_SET_COLOR(idx, rgb.r, rgb.g, rgb.b);
        }
    }
}

/*
 * Draws from `gd` on both halves (not from game_mode_get()), so the mode and the key
 * positions always belong together. Not called at all while RGB is off or suspended:
 * rgb_matrix_task skips the indicator hooks then.
 */
bool game_rgb_indicators(uint8_t led_min, uint8_t led_max) {
    uint8_t mode = gd.flags & GD_MODE_MASK;
    if (mode == GAME_MODE_OFF) return true;
    uint8_t val = rgb_matrix_get_val();

    if (mode == GAME_MODE_CHAT) {
        /* Key LEDs only (the thumb LEDs carry the key flag too, although the thumb keys have no
         * matrix->LED entry); the underglow keeps running the effect. Floored like the trainer's
         * flash: the alarm must stay visible at a very low brightness setting. */
        if (val < GAME_RGB_CHAT_MIN_VAL) val = GAME_RGB_CHAT_MIN_VAL;
        for (uint8_t i = led_min; i < led_max; i++) {
            if (HAS_FLAGS(g_led_config.flags[i], LED_FLAG_KEYLIGHT)) rgb_matrix_set_color(i, val, 0, 0);
        }
        return false;
    }

    gd_light(gd.wasd, GAME_RGB_WASD_HUE, val, led_min, led_max);
    /* Second, so a key that is both (the arrows sit under W/A/S/D) shows the lock. */
    if (gd.flags & GD_NAV_LOCKED) gd_light(gd.arrows, GAME_RGB_ARROWS_HUE, val, led_min, led_max);
    return true;
}
#endif

/* Both hooks below are only called on the master, where game_mode_get() is valid. */

bool mouse_jiggler_inhibited_user(void) {
    return game_mode_get() != GAME_MODE_OFF;
}

#ifdef OLED_ENABLE
bool corne_chat_mode_user(void) {
    return game_mode_get() == GAME_MODE_CHAT;
}
#endif
