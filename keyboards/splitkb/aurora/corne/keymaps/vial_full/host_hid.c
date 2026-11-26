/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Raw HID dispatcher for the keymap-level commands; protocol in host_hid.h. */
#include "quantum.h"
#include "via.h"
#include "host_hid.h"
#include "fw_version.h"
#include "host_status.h"
#include "misfire.h"
#include "trainer.h"

_Static_assert(TRAINER_ITEM_MAX_LEN <= 32 - 3, "a drill item must fit one raw HID report");
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
        default:
            data[0] = id_unhandled;
            break;
    }
}
