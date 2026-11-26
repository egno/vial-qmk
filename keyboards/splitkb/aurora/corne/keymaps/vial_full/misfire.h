/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Passive mod-tap misfire log; hooks are dispatched from user_hooks.c.
 *
 * Watches normal typing (never while the trainer is active) and counts, per
 * matrix position that holds a mod-tap on the base layer, how often the
 * tap/hold decision was probably wrong. "Probably" = the user reached for
 * Backspace right afterwards:
 *
 *   held misfire    The mod-tap settled as HOLD, was the only tracked mod-tap
 *                   held, exactly one other key was pressed while it was down,
 *                   and the first key pressed after its release, within
 *                   MISFIRE_WINDOW_MS of that release, is Backspace.
 *                   (Meant a letter, got a capital / shortcut.)
 *
 *   tapped misfire  The mod-tap settled as TAP, another key was pressed while
 *                   it was still physically down, and Backspace is the next
 *                   key pressed after that other key, or the one after that,
 *                   within MISFIRE_WINDOW_MS of the other key's press.
 *                   (Meant a modifier, got two letters.)
 *
 * Backspace means a resolved Backspace tap: plain or modified KC_BSPC, or a
 * layer-tap / mod-tap whose tap is KC_BSPC and which settled as tap.
 *
 * Limitations - these are heuristics, read the counts as a trend, not a fact:
 *   - Only corrections made with Backspace are seen. A wrong shortcut that is
 *     undone with GUI+Z, Esc or the mouse, or not undone at all, is missed;
 *     so are corrections that start later than the window, or after more
 *     typing than described above.
 *   - Ordinary typos are counted too: fast typing rolls over home-row keys all
 *     the time, so "rolled over a tapped mod-tap, then Backspace" also matches
 *     a plain spelling mistake. Tapped counts are therefore inflated; compare
 *     keys with each other rather than reading absolute numbers.
 *   - A deliberate one-key chord followed by Backspace (Shift+letter, then
 *     changing your mind) counts as a held misfire.
 *   - Holds with two or more tracked mod-taps down together are ignored.
 *   - When a rolled key is itself a tracked mod-tap, only the most recent one
 *     is the candidate.
 *   - Counters are per position, whatever layer the mod-tap event came from
 *     (the home-row positions also carry mod-taps on the Fn and Num layers).
 *   - RAM only: everything is lost on power-down or reset. Counts saturate.
 */
#pragma once

#include "quantum.h"

/* Most base-layer mod-tap positions tracked (9 in the stock vial_full keymap). */
#ifndef MISFIRE_MAX_KEYS
#    define MISFIRE_MAX_KEYS 12
#endif

/* How soon Backspace has to follow to count as a correction (ms). */
#ifndef MISFIRE_WINDOW_MS
#    define MISFIRE_WINDOW_MS 1000
#endif

typedef struct {
    keypos_t pos;             /* matrix position */
    uint16_t held_misfires;   /* saturates at UINT16_MAX */
    uint16_t tapped_misfires; /* saturates at UINT16_MAX */
    uint32_t holds;           /* all presses settled as hold, saturates at UINT32_MAX */
    uint32_t taps;            /* all presses settled as tap, saturates at UINT32_MAX */
} misfire_entry_t;

/* Called from user_hooks.c. misfire_process_record only observes; it never consumes the event. */
void misfire_post_init(void);
void misfire_process_record(uint16_t keycode, keyrecord_t *record);

/* Read API. Entries are in matrix order (left half first) for the keys found at
 * power-up / reset; mod-taps added to the base layer in Vial later are appended. */
uint8_t misfire_count(void);
/* Copies entry `index` (0 .. misfire_count()-1) to *out; false if out of range. */
bool misfire_get(uint8_t index, misfire_entry_t *out);
/* Zeroes all counters and rescans the base layer for mod-tap positions. */
void misfire_reset(void);
