/* Firmware version: git hash and build date of this build.
 *
 * Raw HID: send [FW_VERSION_HID_ID, field]; the reply carries a NUL-terminated
 * string from byte 2. Fields: 0 = QMK_VERSION (git describe), 1 = QMK_BUILDDATE,
 * 2 = QMK_GIT_HASH ('*' suffix when built from uncommitted changes).
 *
 * OLED: both halves show the hash and build date for FW_VERSION_OLED_MS after
 * power-up, so you can check that each half got the new firmware.
 */
#include QMK_KEYBOARD_H
#include "version.h"
#include "via.h"

#ifndef FW_VERSION_HID_ID
#    define FW_VERSION_HID_ID 0xF0
#endif

#ifndef FW_VERSION_OLED_MS
#    define FW_VERSION_OLED_MS 5000
#endif

void raw_hid_receive_kb(uint8_t *data, uint8_t length) {
    if (data[0] != FW_VERSION_HID_ID) {
        data[0] = id_unhandled;
        return;
    }
    static const char *const fields[] = {QMK_VERSION, QMK_BUILDDATE, QMK_GIT_HASH};
    uint8_t                  field    = data[1];
    memset(&data[2], 0, length - 2);
    if (field < ARRAY_SIZE(fields)) {
        strncpy((char *)&data[2], fields[field], length - 3);
    } else {
        data[0] = id_unhandled;
    }
}

#ifdef OLED_ENABLE
/* 270° rotation: 5 columns x 16 rows; a full row wraps by itself, so rows are
 * padded to 5 chars instead of ending in '\n' (which would add a blank row). */
static void write_row(const char *s, uint8_t len) {
    for (uint8_t i = 0; i < 5; i++) {
        oled_write_char(i < len && s[i] ? s[i] : ' ', false);
    }
}

static void render_fw_version(void) {
    static const char hash[] = QMK_GIT_HASH;  /* e.g. "c2502f6d4e" or "c2502f6d4e*" */
    static const char date[] = QMK_BUILDDATE; /* "YYYY-MM-DD-HH:MM:SS" */

    write_row("=====", 5);
    write_row(" FW  ", 5);
    write_row("=====", 5);
    for (uint8_t i = 0; i < sizeof(hash) - 1; i += 5) {
        write_row(&hash[i], sizeof(hash) - 1 - i);
    }
    write_row("", 0);
    write_row(&date[0], 4);  /* YYYY */
    write_row(&date[5], 5);  /* MM-DD */
    write_row(&date[11], 5); /* HH:MM */
}

bool oled_task_user(void) {
    static bool shown = true;
    if (timer_read32() < FW_VERSION_OLED_MS) {
        render_fw_version();
        return false;
    }
    if (shown) {
        shown = false;
        oled_clear();
    }
    return true;
}
#endif
