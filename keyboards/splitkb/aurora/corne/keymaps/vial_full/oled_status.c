/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Right-half (secondary) OLED: typing speed, host text, lock / one-shot status, firmware version.
 * The left half keeps the stock corne.c rendering.
 *
 * The secondary half cannot see most of this state locally, so the master packs it into
 * one small struct and pushes it over a custom split transaction (USER_SYNC_STATUS).
 *
 * Screen is 32x128 px (OLED_ROTATION_270): 5 chars x 16 lines.
 *   0      W 123     words per minute
 *   1-4    graph     WPM history, 1 column per second, 32 s window
 *   5-7    host      text set by the host over raw HID (host_status.c), blank when none
 * Rows 8-12 are indicators: each is drawn (inverted) only while active, blank otherwise.
 *   8      LCK n     layer lock
 *   9      CAPS      caps lock
 *   10     WORD      caps word (only when CAPS_WORD_ENABLE); NKRO (n-key rollover) otherwise
 *   11     SCAG      pending one-shot mods (only the pending letters)
 *   12     OSL n     one-shot layer
 *   13     T 3 12    worst mod-tap (misfire.c): key, held misfires (inverted), tapped misfires;
 *                    counts stop at 99, blank until there is a misfire
 *   14    ab12*     firmware commit (* = built with uncommitted changes)
 *   15     11-26     build date (MM-DD)
 */
#include "quantum.h"
#include "transactions.h"
#include "version.h"
#include "oled_status.h"
#include "host_status.h"
#include "misfire.h"
#include <string.h>

#define STATUS_NONE 0xFF

enum {
    STATUS_CAPS_LOCK = 1 << 0,
    STATUS_CAPS_WORD = 1 << 1,
    STATUS_NKRO      = 1 << 2,
};

typedef struct {
    uint8_t wpm;
    uint8_t locked_layer; // highest locked layer, STATUS_NONE if none
    uint8_t oneshot_layer; // STATUS_NONE if none
    uint8_t oneshot_mods;
    uint8_t flags;
    uint8_t misfire_key; // label of the mod-tap with most misfires, 0 if none yet
    uint8_t misfire_held; // its counts, capped at 99
    uint8_t misfire_tapped;
} status_t;
_Static_assert(sizeof(status_t) <= RPC_M2S_BUFFER_SIZE, "status message too big for split RPC");

static status_t status = {.locked_layer = STATUS_NONE, .oneshot_layer = STATUS_NONE};

static void status_slave_handler(uint8_t in_len, const void *in_data, uint8_t out_len, void *out_data) {
    if (in_len == sizeof(status)) {
        memcpy(&status, in_data, sizeof(status));
    }
}

void status_post_init(void) {
    transaction_register_rpc(USER_SYNC_STATUS, status_slave_handler);
}

// Worst mod-tap by held + tapped misfires, labelled with its base-layer tap key.
static void collect_misfire(status_t *s) {
    misfire_entry_t e, worst = {0};
    uint32_t        worst_sum = 0;
    for (uint8_t i = 0; misfire_get(i, &e); i++) {
        uint32_t sum = (uint32_t)e.held_misfires + e.tapped_misfires;
        if (sum > worst_sum) {
            worst_sum = sum;
            worst     = e;
        }
    }
    if (worst_sum == 0) {
        return;
    }
    uint8_t tap = QK_MOD_TAP_GET_TAP_KEYCODE(keymap_key_to_keycode(0, worst.pos));
    s->misfire_key    = (tap >= KC_A && tap <= KC_Z) ? 'A' + (tap - KC_A) : '?';
    s->misfire_held   = worst.held_misfires > 99 ? 99 : worst.held_misfires;
    s->misfire_tapped = worst.tapped_misfires > 99 ? 99 : worst.tapped_misfires;
}

static status_t collect_status(void) {
    status_t s = {.locked_layer = STATUS_NONE, .oneshot_layer = STATUS_NONE};
    collect_misfire(&s);
#ifdef WPM_ENABLE
    s.wpm = get_current_wpm();
#endif
#ifdef LAYER_LOCK_ENABLE
    for (int8_t layer = MAX_LAYER - 1; layer >= 0; layer--) {
        if (is_layer_locked(layer)) {
            s.locked_layer = layer;
            break;
        }
    }
#endif
#ifndef NO_ACTION_ONESHOT
    s.oneshot_mods = get_oneshot_mods() | get_oneshot_locked_mods();
    if (get_oneshot_layer_state()) {
        s.oneshot_layer = get_oneshot_layer();
    }
#endif
    if (host_keyboard_led_state().caps_lock) s.flags |= STATUS_CAPS_LOCK;
#ifdef CAPS_WORD_ENABLE
    if (is_caps_word_on()) s.flags |= STATUS_CAPS_WORD;
#endif
#ifdef NKRO_ENABLE
    if (keymap_config.nkro) s.flags |= STATUS_NKRO;
#endif
    return s;
}

void status_housekeeping(void) {
    if (!is_keyboard_master()) {
        return;
    }
    static status_t last_sent;
    static uint32_t last_sync = 0;
    status_t        now = collect_status();
    // Push on change, and periodically so a re-plugged secondary catches up.
    if (memcmp(&now, &last_sent, sizeof(now)) != 0 || timer_elapsed32(last_sync) > 500) {
        if (transaction_rpc_send(USER_SYNC_STATUS, sizeof(now), &now)) {
            last_sent = now;
            last_sync = timer_read32();
        }
    }
}

#ifdef OLED_ENABLE

#    define GRAPH_W 32
#    define GRAPH_H 32
#    define GRAPH_TOP 8 // pixel row (text line 1)
#    define GRAPH_SAMPLE_MS 1000
#    define GRAPH_WPM_PER_PX 4 // full height = 128 WPM

static uint8_t graph[GRAPH_W]; // bar heights, oldest first

static void line(uint8_t row, const char *text, bool invert) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%-5s", text);
    oled_set_cursor(0, row);
    oled_write(buf, invert);
}

// Indicators are only drawn while active; the row stays blank otherwise.
static void flag(uint8_t row, const char *text, bool active) {
    line(row, active ? text : "", active);
}

static void render_wpm(void) {
    char buf[6];
    snprintf(buf, sizeof(buf), "W%4u", status.wpm);
    line(0, buf, false);

    static uint32_t last_sample = 0;
    if (timer_elapsed32(last_sample) >= GRAPH_SAMPLE_MS) {
        last_sample = timer_read32();
        memmove(graph, graph + 1, GRAPH_W - 1);
        uint8_t h           = status.wpm / GRAPH_WPM_PER_PX;
        graph[GRAPH_W - 1] = h > GRAPH_H ? GRAPH_H : h;
    }
    for (uint8_t x = 0; x < GRAPH_W; x++) {
        for (uint8_t y = 0; y < GRAPH_H; y++) {
            // y counts down from the bottom of the graph; dotted baseline keeps the area visible.
            bool on = (y < graph[x]) || (y == 0 && x % 2 == 0);
            oled_write_pixel(x, GRAPH_TOP + GRAPH_H - 1 - y, on);
        }
    }
}

static void render_locks(void) {
    char buf[6];
    if (status.locked_layer != STATUS_NONE) {
        snprintf(buf, sizeof(buf), "LCK%2u", status.locked_layer);
        line(8, buf, true);
    } else {
        line(8, "", false);
    }
    flag(9, "CAPS", status.flags & STATUS_CAPS_LOCK);
    // Caps word is the transient one, so it wins the shared row.
    flag(10, (status.flags & STATUS_CAPS_WORD) ? "WORD" : "NKRO", status.flags & (STATUS_CAPS_WORD | STATUS_NKRO));
}

static void render_oneshot(void) {
    static const struct {
        char    c;
        uint8_t mask;
    } mods[] = {{'S', MOD_MASK_SHIFT}, {'C', MOD_MASK_CTRL}, {'A', MOD_MASK_ALT}, {'G', MOD_MASK_GUI}};
    oled_set_cursor(0, 11);
    oled_write_char(' ', false);
    for (uint8_t i = 0; i < ARRAY_SIZE(mods); i++) {
        bool on = status.oneshot_mods & mods[i].mask;
        oled_write_char(on ? mods[i].c : ' ', on);
    }

    char buf[6];
    if (status.oneshot_layer != STATUS_NONE) {
        snprintf(buf, sizeof(buf), "OSL%2u", status.oneshot_layer);
        line(12, buf, true);
    } else {
        line(12, "", false);
    }
}

static void render_misfire(void) {
    if (!status.misfire_key) {
        line(13, "", false);
        return;
    }
    char buf[3];
    oled_set_cursor(0, 13);
    oled_write_char(status.misfire_key, false);
    snprintf(buf, sizeof(buf), "%2u", status.misfire_held);
    oled_write(buf, true);
    snprintf(buf, sizeof(buf), "%2u", status.misfire_tapped);
    oled_write(buf, false);
}

static void render_version(void) {
    // QMK_GIT_HASH is the commit hash, with a trailing '*' for a dirty tree (same string fw_version.c reports).
    static const char hash[] = QMK_GIT_HASH;
    char              ver[6];
    snprintf(ver, sizeof(ver), "%.4s%s", hash, (sizeof(hash) > 1 && hash[sizeof(hash) - 2] == '*') ? "*" : "");
    line(14, ver, false);

    // QMK_BUILDDATE is "YYYY-MM-DD-hh:mm:ss"; show MM-DD.
    char date[6];
    memcpy(date, QMK_BUILDDATE + 5, 5);
    date[5] = '\0';
    line(15, date, false);
}

bool status_oled_task(void) {
    if (is_keyboard_master()) {
        return true; // stock corne.c status screen
    }
    static bool cleared = false;
    if (!cleared) {
        oled_clear(); // drop the stock artwork once
        cleared = true;
    }
    render_wpm();
    host_status_render(5);
    render_locks();
    render_oneshot();
    render_misfire();
    render_version();
    return false;
}

#endif
