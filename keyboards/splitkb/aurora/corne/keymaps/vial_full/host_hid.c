/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Raw HID dispatcher for the keymap-level commands; protocol in host_hid.h. */
#include "quantum.h"
#include "via.h"
#include "host_hid.h"
#include "fw_version.h"
#include "host_status.h"
#include "hrm.h"
#include "misfire.h"
#include "trainer.h"

_Static_assert(TRAINER_ITEM_MAX_LEN <= 32 - 3, "a drill item must fit one raw HID report");
_Static_assert(TRAINER_ITEM_MAX_LEN <= 32 - 6, "a drill item text must fit one 0xF5 reply");
_Static_assert(MATRIX_ROWS * MATRIX_COLS <= 48, "0xF6 packs four 48-bit matrix masks into one report");
_Static_assert(HOST_STATUS_ROWS * HOST_STATUS_COLS <= 32 - 3, "the host text must fit one raw HID report");

static void drills_hid(uint8_t *data, uint8_t length) {
    uint8_t status = TRAINER_HOST_OK, bad_pos = 0, bad_char = 0;
    switch (data[1]) {
        case HOST_HID_DRILLS_INFO:
            break;
        case HOST_HID_DRILLS_CLEAR:
            trainer_host_clear();
            break;
        case HOST_HID_DRILLS_ADD: {
            uint8_t len = data[2];
            if (len > length - 3) {
                status = TRAINER_HOST_ERR_LENGTH;
            } else {
                status = trainer_host_add((const char *)&data[3], len, &bad_pos);
                if (status == TRAINER_HOST_ERR_CHAR) bad_char = data[3 + bad_pos];
            }
            break;
        }
        case HOST_HID_DRILLS_MODE:
            trainer_host_set_only(data[2] != 0);
            break;
        default:
            data[0] = id_unhandled;
            return;
    }
    memset(&data[2], 0, length - 2);
    data[2] = status;
    data[3] = TRAINER_HOST_ITEMS;
    data[4] = trainer_host_count();
    data[5] = TRAINER_ITEM_MAX_LEN;
    data[6] = trainer_host_only();
    data[7] = bad_pos;
    data[8] = bad_char;
}

static void status_hid(uint8_t *data, uint8_t length) {
    host_status_set((const char *)&data[3], data[2], data[1]);
    memset(&data[2], 0, length - 2);
    data[3] = HOST_STATUS_ROWS;
    data[4] = HOST_STATUS_COLS;
}

static uint8_t keymap_layer_count_hid(void) {
#ifdef DYNAMIC_KEYMAP_ENABLE
    return dynamic_keymap_get_layer_count();
#else
    return keymap_layer_count();
#endif
}

static uint8_t *put_le(uint8_t *p, uint32_t v, uint8_t bytes) {
    while (bytes--) {
        *p++ = v & 0xFF;
        v >>= 8;
    }
    return p;
}

static void misfire_hid(uint8_t *data, uint8_t length) {
    uint8_t         sub = data[1], index = data[2];
    misfire_entry_t e;
    bool            found = false;
    switch (sub) {
        case HOST_HID_MISFIRE_GET:
            found = misfire_get(index, &e);
            break;
        case HOST_HID_MISFIRE_RESET:
            misfire_reset();
            index = 0;
            break;
        default:
            data[0] = id_unhandled;
            return;
    }
    memset(&data[2], 0, length - 2);
    data[2] = (sub == HOST_HID_MISFIRE_GET && !found) ? 1 : 0;
    data[3] = misfire_count();
    data[4] = index;
    if (found) {
        uint8_t *p = &data[5];
        *p++       = e.pos.row;
        *p++       = e.pos.col;
        p          = put_le(p, keymap_key_to_keycode(0, e.pos), 2);
        p          = put_le(p, e.held_misfires, 2);
        p          = put_le(p, e.tapped_misfires, 2);
        p          = put_le(p, e.holds, 4);
        put_le(p, e.taps, 4);
    }
}

/* 0xF4: everything here is the master's own state (layers, mods, host LEDs,
 * trainer); nothing comes from the other half. */
static void state_hid(uint8_t *data, uint8_t length) {
    memset(&data[2], 0, length - 2);
    data[1]    = 0;
    data[3]    = get_highest_layer(layer_state | default_layer_state);
    uint8_t *p = put_le(&data[4], layer_state, 4);
    p          = put_le(p, default_layer_state, 4);
    uint32_t locked = 0;
#ifdef LAYER_LOCK_ENABLE
    for (uint8_t layer = 0; layer < MAX_LAYER; layer++) {
        if (is_layer_locked(layer)) locked |= (uint32_t)1 << layer;
    }
#endif
    put_le(p, locked, 4);

    uint8_t flags = 0;
    if (trainer_is_active()) flags |= HOST_HID_STATE_TRAINER;
#ifdef CAPS_WORD_ENABLE
    if (is_caps_word_on()) flags |= HOST_HID_STATE_CAPS_WORD;
#endif
    if (host_keyboard_led_state().caps_lock) flags |= HOST_HID_STATE_CAPS_LOCK;
    data[22] = 0xFF;
#ifndef NO_ACTION_ONESHOT
    data[20] = get_oneshot_mods();
    data[21] = get_oneshot_locked_mods();
    if (get_oneshot_layer_state()) {
        flags |= HOST_HID_STATE_ONESHOT_LAYER;
        data[22] = get_oneshot_layer();
    }
#endif
    data[16] = flags;
    data[17] = trainer_state();
    put_le(&data[18], trainer_boot_sessions(), 2);
    data[23] = get_mods();
    data[24] = get_weak_mods();
    data[25] = keymap_layer_count_hid();
}

static void trainer_hid(uint8_t *data, uint8_t length) {
    uint8_t sub = data[1], index = data[2];
    if (sub > HOST_HID_TRAINER_TEXT) {
        data[0] = id_unhandled;
        return;
    }
    memset(&data[2], 0, length - 2);
    uint8_t *p = &data[3];
    switch (sub) {
        case HOST_HID_TRAINER_SUMMARY: {
            trainer_summary_t s;
            trainer_get_summary(&s);
            data[2] = trainer_boot_sessions() == 0 && s.items == 0;
            *p++    = trainer_state();
            p       = put_le(p, trainer_boot_sessions(), 2);
            *p++    = s.wpm;
            *p++    = s.accuracy;
            *p++    = s.items;
            p       = put_le(p, s.correct, 2);
            p       = put_le(p, s.errors, 2);
            for (uint8_t k = 0; k < TRAINER_ERR_KINDS; k++) p = put_le(p, s.err_kind[k], 2);
            p    = put_le(p, s.active_ms, 4);
            *p++ = s.best_wpm;
            *p++ = s.best_accuracy;
            p    = put_le(p, s.total_sessions, 2);
            *p++ = TRAINER_CHARS;
            *p++ = trainer_item_count();
            break;
        }
        case HOST_HID_TRAINER_CHARS: {
            trainer_stat_t st;
            uint8_t        n = 0;
            data[3]          = TRAINER_CHARS;
            data[4]          = index;
            p                = &data[6];
            while (n < 3 && index + n < TRAINER_CHARS && trainer_char_stat(index + n, &st)) {
                p = put_le(p, st.count, 2);
                p = put_le(p, st.errors, 2);
                p = put_le(p, st.total_ms, 4);
                n++;
            }
            data[5] = n;
            data[2] = n == 0;
            break;
        }
        default: { /* HOST_HID_TRAINER_ITEM, HOST_HID_TRAINER_TEXT */
            trainer_stat_t st;
            uint8_t        group;
            const char *   text;
            data[3] = trainer_item_count();
            data[4] = index;
            if (!trainer_item_stat(index, &st, &group, &text)) {
                data[2] = 1;
                break;
            }
            uint8_t len = strlen(text);
            if (sub == HOST_HID_TRAINER_TEXT) {
                data[5] = len;
                memcpy(&data[6], text, len);
            } else {
                data[5] = group;
                data[6] = len;
                p       = put_le(&data[7], st.count, 2);
                p       = put_le(p, st.errors, 2);
                put_le(p, st.total_ms, 4);
            }
            break;
        }
    }
}

static void mask_set(uint8_t *mask, uint8_t pos) {
    mask[pos / 8] |= 1 << (pos % 8);
}

static void taphold_hid(uint8_t *data, uint8_t length) {
    uint8_t layer = data[1];
    memset(&data[2], 0, length - 2);
    bool current = layer == HOST_HID_TAPHOLD_LAYER_CURRENT;
    bool valid   = current || layer < keymap_layer_count_hid();
    data[2]      = !valid;

    uint16_t term = hrm_flow_tap_term();
    if (hrm_chordal_hold_enabled()) data[3] |= HOST_HID_TAPHOLD_CHORDAL;
    if (term) data[3] |= HOST_HID_TAPHOLD_FLOW;
    put_le(&data[4], term, 2);
    data[6] = MATRIX_ROWS;
    data[7] = MATRIX_COLS;

    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            keypos_t pos  = {.row = r, .col = c};
            uint8_t  bit  = r * MATRIX_COLS + c;
            char     hand = hrm_chordal_hand(pos);
            if (hand == '*') mask_set(&data[8], bit);
            if (hand == 'R') mask_set(&data[14], bit);
            if (!valid) continue;
            /* Read-only: asks which layer a press would resolve to, without touching the source-layer cache. */
            uint16_t kc = keymap_key_to_keycode(current ? layer_switch_get_layer(pos) : layer, pos);
            if (IS_QK_MOD_TAP(kc) || IS_QK_LAYER_TAP(kc)) mask_set(&data[20], bit);
            if (hrm_flow_tap_keycode(kc)) mask_set(&data[26], bit);
        }
    }
}

void raw_hid_receive_kb(uint8_t *data, uint8_t length) {
    /* VIA also routes a few of its own ids here (custom channels); they stay unhandled. */
    if (length < 32) {
        data[0] = id_unhandled;
        return;
    }
    switch (data[0]) {
        case HOST_HID_VERSION:
            fw_version_hid(data, length);
            break;
        case HOST_HID_DRILLS:
            drills_hid(data, length);
            break;
        case HOST_HID_STATUS:
            status_hid(data, length);
            break;
        case HOST_HID_MISFIRE:
            misfire_hid(data, length);
            break;
        case HOST_HID_STATE:
            state_hid(data, length);
            break;
        case HOST_HID_TRAINER:
            trainer_hid(data, length);
            break;
        case HOST_HID_TAPHOLD:
            taphold_hid(data, length);
            break;
        default:
            data[0] = id_unhandled;
            break;
    }
}
