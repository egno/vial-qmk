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
 * 0xF4-0xF6 are read-only and pull-only: the host polls. The keyboard never
 * sends a report on its own (Vial may be using the same interface) unless a
 * host asked for it with 0xF7. All multi-byte fields are little-endian. Matrix positions are numbered
 * row * 6 + col (8 rows x 6 columns: rows 0-3 left half, 4-7 right half;
 * 3,0-3,2 and 7,0-7,2 have no key); a 48-bit "matrix mask" is 6 bytes, bit
 * (pos % 8) of byte (pos / 8) is position pos. Mod masks are QMK's 8-bit
 * form: bit 0 LCtrl, 1 LShift, 2 LAlt, 3 LGUI, 4 RCtrl, 5 RShift, 6 RAlt,
 * 7 RGUI.
 *
 * 0xF4 HOST_HID_STATE     [0xF4]   live state, cheap enough to poll at 10-20 Hz
 *      Reply, by byte offset (bytes 26-31 are 0):
 *       0     0xF4
 *       1     0 (request byte 1 echoed)
 *       2     status, always 0
 *       3     highest active layer = highest bit set in layer_state | default_layer_state
 *       4-7   layer_state, u32 (bit n = layer n on; the default layer is not in here)
 *       8-11  default_layer_state, u32
 *       12-15 locked layers (Layer Lock), u32, bit n = layer n locked
 *       16    flags: bit 0 trainer active (trainer state != 0), bit 1 Caps Word on,
 *             bit 2 Caps Lock (host LED state), bit 3 a one-shot layer is pending
 *       17    trainer state: 0 off, 1 session running, 2 results screen,
 *             3 on but no drill item is typeable on this keymap
 *       18-19 trainer sessions finished since power-up, u16, wraps. When it
 *             changes there is a new last-session summary to fetch with 0xF5.
 *       20    one-shot mods pending (mod mask)
 *       21    one-shot mods locked (mod mask)
 *       22    pending one-shot layer, 0xFF if none
 *       23    mods currently held (mod mask, get_mods(): real mods and settled
 *             mod-tap holds; normally 0 while the trainer is active, which
 *             keeps mods to itself)
 *       24    weak mods (mod mask; the Shift of Caps Word, LSFT(kc) keys, ...)
 *       25    number of keymap layers
 *
 * 0xF5 HOST_HID_TRAINER   [0xF5, sub, index]   trainer statistics (trainer.c, RAM only)
 *      Everything except "best" and "sessions ever" is lost at power-down.
 *      sub 0 summary [0xF5, 0]          last finished session + all-time record
 *      sub 1 chars   [0xF5, 1, index]   per-character stats, 3 characters per reply
 *      sub 2 item    [0xF5, 2, index]   per-item stats of drill item `index`
 *      sub 3 text    [0xF5, 3, index]   text of drill item `index`
 *
 *      Reply sub 0, by byte offset (byte 31 is 0):
 *       0-1   0xF5, 0
 *       2     status: 0 ok, 1 no session was finished since power-up (bytes
 *             6-24 are 0; everything else is valid)
 *       3     trainer state, as byte 17 of 0xF4
 *       4-5   sessions finished since power-up, u16, wraps (= bytes 18-19 of 0xF4)
 *       --- last finished session (a session is `items` drill items) ---
 *       6     speed, WPM = correct keystrokes / 5 per minute of active time, capped at 255
 *       7     accuracy, % = 100 * correct / (correct + misses), rounded down
 *       8     items completed
 *       9-10  correct keystrokes, u16
 *       11-12 misses, u16 (= sum of the four kinds below)
 *       13-14 misses "wrong key", u16
 *       15-16 misses "held": a tap was wanted, the key settled as hold (or a
 *             stray modifier was down), u16
 *       17-18 misses "tapped": a hold (mod / layer) was wanted, the key settled as tap, u16
 *       19-20 misses "wrong layer": right physical key, wrong layer active, u16
 *       21-24 active time in ms, u32: sum of the latencies of the correct
 *             keystrokes, each capped at 5000 ms
 *       --- all time (EEPROM) ---
 *       25    best WPM of any session
 *       26    best accuracy of any session, % (independent of byte 25)
 *       27-28 sessions finished ever, u16, saturates at 8191
 *       --- sizes for the paged sub-commands ---
 *       29    number of characters for sub 1 (95)
 *       30    number of drill items for sub 2/3 (built-in + loaded host items)
 *
 *      Reply sub 1: [0xF5, 1, status, total, index, n, n x {correct(2), misses(2), latency_ms(4)}]
 *       n (0..3) records from byte 6, 8 bytes each, for the characters with
 *       ASCII code 0x20 + index, 0x20 + index + 1, ...; total = 95 (0x20-0x7E).
 *       Fetch all with index = 0, 3, 6, ... while index < total. correct =
 *       times the character was typed right when it was the expected one,
 *       misses = wrong keystrokes while it was the expected one (attempts =
 *       correct + misses), latency_ms = sum over the correct keystrokes of
 *       the time since the previous correct keystroke or the start of the
 *       item, each capped at 5000 ms (mean latency = latency_ms / correct).
 *       Counters are cumulative since power-up and saturate at 65535; they
 *       also grow during a running session.
 *       status: 0 ok, 1 index >= total (n = 0).
 *
 *      Reply sub 2: [0xF5, 2, status, count, index, group, len, completions(2), misses(2), time_ms(4)]
 *       count = number of drill items (index 0..count-1): the built-in ones
 *       first, then the host items of 0xF1 in the order they were added (a
 *       0xF1 clear removes them and their stats). group: 0 pairs, 1 snippets,
 *       2 numbers, 3 host. len = length of the text. completions = times the
 *       item was typed to the end, misses = wrong keystrokes in completed
 *       runs, time_ms = sum of the active time of the completed runs.
 *       Cumulative since power-up.
 *       status: 0 ok, 1 index out of range (bytes from group on are 0).
 *
 *      Reply sub 3: [0xF5, 3, status, count, index, len, text(len)]
 *       text is printable ASCII from byte 6, len <= 24, not NUL-terminated
 *       beyond the zero padding of the report.
 *       status: 0 ok, 1 index out of range (len = 0).
 *
 * 0xF6 HOST_HID_TAPHOLD   [0xF6, layer]   Chordal Hold / Flow Tap per key (hrm.c)
 *      layer 0..layers-1: look at the keycodes stored on that layer; a
 *      transparent key has neither bit set in the two keycode masks (ask the
 *      layer below). layer 0xFF: look at the keycode each position resolves
 *      to with the layers active right now.
 *      Reply, by byte offset:
 *       0     0xF6
 *       1     layer, echoed
 *       2     status: 0 ok, 1 no such layer (bytes 20-31 are 0, the rest is valid)
 *       3     flags: bit 0 Chordal Hold is on, bit 1 Flow Tap is on (term > 0).
 *             Both are Vial QMK Settings and can change at run time.
 *       4-5   Flow Tap term in ms as applied, u16 (0 = off, at most 500)
 *       6     matrix rows (8)
 *       7     matrix columns (6)
 *       8-13  matrix mask "chordal exempt": the key is '*' in chordal_hold_layout
 *       14-19 matrix mask "chordal right hand": the key is 'R'; a key in
 *             neither mask is 'L'. These two do not depend on the layer.
 *       20-25 matrix mask "tap-hold": the keycode is a mod-tap or layer-tap
 *       26-31 matrix mask "flow tap": the keycode takes part in Flow Tap
 *      Chordal Hold (when on): a tap-hold key chorded with a key of the same
 *      hand settles as tap, unless either of the two keys is exempt; with
 *      opposite hands or an exempt key the usual tap-hold rules decide.
 *      Flow Tap (when on): a tap-hold key in the flow tap mask, pressed less
 *      than `term` ms after a key that is also in the mask, is a tap at once.
 *      A tap-hold key outside the flow tap mask is never forced to tap, and
 *      a key outside it does not arm Flow Tap for the next key. On top of the
 *      mask Flow Tap is off while Ctrl, GUI or left Alt is held.
 *
 * 0xF7 HOST_HID_EVENTS    [0xF7, sub, ...]   push instead of poll: state and pressed keys
 *      sub 0 subscribe [0xF7, 0, ttl_s]   ttl_s 1..255 seconds; 0 unsubscribes
 *      sub 1 state     [0xF7, 1]          the current state event, as a reply
 *      sub 2 matrix    [0xF7, 2]          the current matrix event, as a reply
 *      Sub 1 and 2 work without a subscription and change nothing.
 *
 *      Reply sub 0: [0xF7, 0, status (0), ttl_s as applied], the rest 0.
 *      There is one subscription, not one per host program. It ends ttl_s
 *      seconds after the last sub 0 request, so the host repeats the request
 *      well inside that time (e.g. every ttl_s / 3); it also ends at once on
 *      ttl_s 0 and on any USB suspend, reset or reconfiguration. The device is
 *      usually open in several programs at once (Vial), so the reports have
 *      to stop by themselves when the subscriber dies.
 *
 *      While subscribed the keyboard sends the two reports below unasked.
 *      They are told from replies by byte 1 only; a reply to sub 1 / sub 2 and
 *      the event of that kind are the same bytes. Every sub 0 request with
 *      ttl_s > 0 (renewals too) is followed by one event of each kind with
 *      the current values, changed or not, after the reply. Changes are
 *      looked for once per main loop pass (host_hid_housekeeping), so a pass
 *      sends at most one event of each kind and what happened inside one pass
 *      is seen as its end result.
 *
 *      State event, by byte offset (bytes 26-31 are 0):
 *       0     0xF7
 *       1     1
 *       2-25  bytes 2-25 of the 0xF4 reply, bit for bit (byte 2 = status 0,
 *             byte 3 = highest layer, ... byte 25 = number of layers)
 *      Sent when any of bytes 2-25 differs from the state event sent last.
 *      None of these fields moves without the user or the host doing
 *      something; still, state events are kept at least 20 ms apart, and the
 *      state at the end of a burst is always sent.
 *
 *      Matrix event, by byte offset (bytes 11-31 are 0):
 *       0     0xF7
 *       1     2
 *       2     status, always 0
 *       3     matrix rows (8)
 *       4     matrix columns (6)
 *       5-10  matrix mask (see above; the same packing as in 0xF6) of the keys
 *             physically down, both halves, after debouncing. Byte 5 bit 0
 *             is row 0 col 0; read as a little-endian 48-bit number, bit
 *             row * 6 + col.
 *      Sent when the mask differs from the one sent last; not rate limited.
 *      It shows switches, not keycodes: it does not depend on layers and
 *      also reports keys the trainer swallows.
 *
 *      Privacy: the matrix event tells every program that has the raw HID
 *      interface open which keys are down, i.e. together with the keymap
 *      (readable over VIA) what is being typed, without any OS-level input
 *      monitoring permission. That is why it is off until a host subscribes
 *      and why the subscription expires. Vial and VIA tools pair requests
 *      and replies by order only and can take an event for their reply, so
 *      do not keep a subscription running while editing the keymap in Vial.
 */
#pragma once

#include <stdbool.h>

enum host_hid_command {
    HOST_HID_VERSION = 0xF0,
    HOST_HID_DRILLS  = 0xF1,
    HOST_HID_STATUS  = 0xF2,
    HOST_HID_MISFIRE = 0xF3,
    HOST_HID_STATE   = 0xF4,
    HOST_HID_TRAINER = 0xF5,
    HOST_HID_TAPHOLD = 0xF6,
    HOST_HID_EVENTS  = 0xF7,
};

/* 0xF7 sub-commands; 1 and 2 are also byte 1 of the unsolicited reports. */
enum host_hid_events_sub {
    HOST_HID_EVENTS_SUBSCRIBE,
    HOST_HID_EVENTS_STATE,
    HOST_HID_EVENTS_MATRIX,
};

/* Hooks, dispatched from user_hooks.c. */
void host_hid_housekeeping(void);
void host_hid_usb_state(bool configured);

enum host_hid_trainer_sub {
    HOST_HID_TRAINER_SUMMARY,
    HOST_HID_TRAINER_CHARS,
    HOST_HID_TRAINER_ITEM,
    HOST_HID_TRAINER_TEXT,
};

/* 0xF4 flags (byte 16). */
enum host_hid_state_flags {
    HOST_HID_STATE_TRAINER       = 1 << 0,
    HOST_HID_STATE_CAPS_WORD     = 1 << 1,
    HOST_HID_STATE_CAPS_LOCK     = 1 << 2,
    HOST_HID_STATE_ONESHOT_LAYER = 1 << 3,
};

/* 0xF6 flags (byte 3) and the layer value for "as resolved right now". */
enum host_hid_taphold_flags {
    HOST_HID_TAPHOLD_CHORDAL = 1 << 0,
    HOST_HID_TAPHOLD_FLOW    = 1 << 1,
};
#define HOST_HID_TAPHOLD_LAYER_CURRENT 0xFF

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
