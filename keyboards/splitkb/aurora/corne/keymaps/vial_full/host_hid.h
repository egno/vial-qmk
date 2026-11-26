/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Keymap-level raw HID commands (host_hid.c owns raw_hid_receive_kb).
 *
 * Transport: the QMK raw HID interface (usage page 0xFF60, usage 0x61),
 * 32-byte reports, one reply per request in the same buffer. VIA/Vial see
 * every report first; VIA uses command ids 0x01-0x13, Vial 0xFE, and 0xFF
 * means "unhandled", so this keymap keeps to 0xF0-0xF7. Host tool:
 * tools/corne_host/corne_host.py.
 *
 * Unless noted a reply echoes bytes 0 and 1 and carries a status in byte 2
 * (0 = ok). An unknown command, sub-command or field is answered with
 * byte 0 = 0xFF. Unused request bytes should be 0.
 *
 * 0xF0 HOST_HID_VERSION   [0xF0, field]
 *      field 0 = QMK_VERSION, 1 = QMK_BUILDDATE, 2 = QMK_GIT_HASH.
 *      Reply [0xF0, field, string..., 0]: NUL-terminated from byte 2 (no
 *      status byte; older builds answer this command the same way).
 *
 * 0xF1 HOST_HID_DRILLS    [0xF1, sub, ...]   host-fed trainer items (RAM only)
 *      sub 0 info    [0xF1, 0]
 *      sub 1 clear   [0xF1, 1]                 drop all host items and their stats
 *      sub 2 add     [0xF1, 2, len, text...]   append one item, len 1..max_len
 *      sub 3 mode    [0xF1, 3, only]           1 = drill host items only, 0 = mix
 *      Reply (all subs) [0xF1, sub, status, capacity, count, max_len, only, bad_pos, bad_char]
 *      status: 0 ok, 1 full, 2 bad length, 3 character at bad_pos (bad_char)
 *      is not printable ASCII or cannot be typed on the current keymap,
 *      4 duplicate of a loaded item. Rejected items change nothing.
 *      "only" takes effect while at least one host item is typeable;
 *      otherwise the built-in items are used as if it were 0.
 *
 * 0xF2 HOST_HID_STATUS    [0xF2, ttl_s, invert, text(15)]   right-OLED host text
 *      text is HOST_STATUS_ROWS rows of HOST_STATUS_COLS chars, row after row,
 *      padded with spaces or NULs; a blank row hides that row. invert bit n
 *      draws row n inverted. The text is dropped ttl_s seconds after the last
 *      report (so it disappears when the host script dies); ttl_s 0 clears it
 *      at once.
 *      Reply [0xF2, ttl_s, 0, rows, cols].
 *
 * 0xF3 HOST_HID_MISFIRE   [0xF3, sub, index]   mod-tap misfire log (misfire.c, RAM only)
 *      sub 0 get     [0xF3, 0, index]   one tracked key, index 0..count-1
 *      sub 1 reset   [0xF3, 1]          zero all counters, rescan the base layer
 *      Reply [0xF3, sub, status, count, index, row, col, keycode(2),
 *             held_misfires(2), tapped_misfires(2), holds(4), taps(4)]
 *      status: 0 ok, 1 index out of range (bytes from row on are 0; count is
 *      still valid, so "get 0" doubles as the count query). Multi-byte fields
 *      are little-endian; keycode is the key's current base-layer keycode.
 *
 * 0xF4-0xF7 free.
 */
#pragma once

enum host_hid_command {
    HOST_HID_VERSION = 0xF0,
    HOST_HID_DRILLS  = 0xF1,
    HOST_HID_STATUS  = 0xF2,
    HOST_HID_MISFIRE = 0xF3,
};

enum host_hid_misfire_sub {
    HOST_HID_MISFIRE_GET,
    HOST_HID_MISFIRE_RESET,
};

enum host_hid_drills_sub {
    HOST_HID_DRILLS_INFO,
    HOST_HID_DRILLS_CLEAR,
    HOST_HID_DRILLS_ADD,
    HOST_HID_DRILLS_MODE,
};
