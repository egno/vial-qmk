/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * On-board layer/symbol drill ("typing trainer") for the vial_full keymap.
 *
 * Toggle with TRAINER_TOGGLE (QK_KB_1, "Trainer" in Vial's User tab; placed on
 * the mouse layer next to the jiggler). While active, nothing reaches the
 * host: every key is consumed by the trainer, except layer keys
 * (MO/LT-hold/TG/...) which still switch layers so you can drill them.
 * The trainer switches itself off after TRAINER_IDLE_EXIT_MS without input.
 */
#pragma once

#include "quantum.h"

/* Custom keycode. Vial's customKeycodes[i] maps to QK_KB_i; slot 0 is the mouse jiggler (corne.c). */
#define TRAINER_TOGGLE QK_KB_1

/* Items per session before the results screen / heatmap is shown. */
#ifndef TRAINER_SESSION_ITEMS
#    define TRAINER_SESSION_ITEMS 12
#endif

/* Red full-board flash length on a miss (ms). */
#ifndef TRAINER_FLASH_MS
#    define TRAINER_FLASH_MS 160
#endif

/* Latencies above this (ms) are clamped so going AFK doesn't wreck stats. */
#ifndef TRAINER_LATENCY_CAP_MS
#    define TRAINER_LATENCY_CAP_MS 5000
#endif

/* Leave trainer mode after this long without any key activity (ms); 0 = never.
 * Matches the default OLED timeout, so the keyboard is never silently "dead"
 * to the host once the drill screen has gone dark. */
#ifndef TRAINER_IDLE_EXIT_MS
#    define TRAINER_IDLE_EXIT_MS 60000
#endif

/* The results screen ignores taps for this long, so typing momentum from the
 * last item does not dismiss it (ms). */
#ifndef TRAINER_RESULTS_HOLD_MS
#    define TRAINER_RESULTS_HOLD_MS 1200
#endif

/* Longest drill item, built-in or host-supplied (chars). */
#define TRAINER_ITEM_MAX_LEN 24

/* Slots for drill items pushed by the host over raw HID (RAM only). */
#ifndef TRAINER_HOST_ITEMS
#    define TRAINER_HOST_ITEMS 64
#endif

bool trainer_is_active(void);
void trainer_toggle(void);

/* ---- Read-only exports for the host (host_hid.c, commands 0xF4/0xF5). ---- */

/* Wire values, see host_hid.h. */
enum trainer_state {
    TRAINER_STATE_OFF,
    TRAINER_STATE_RUN,
    TRAINER_STATE_RESULTS,
    TRAINER_STATE_EMPTY, /* on, but no drill item is typeable on this keymap */
};
uint8_t trainer_state(void);
/* Sessions finished since power-up (wraps). */
uint16_t trainer_boot_sessions(void);

#define TRAINER_ERR_KINDS 4 /* wrong key, held, tapped, wrong layer */
typedef struct {
    /* last finished session; all 0 until one was finished since power-up */
    uint8_t  wpm, accuracy, items;
    uint16_t correct, errors;
    uint16_t err_kind[TRAINER_ERR_KINDS];
    uint32_t active_ms;
    /* all time, from EEPROM */
    uint8_t  best_wpm, best_accuracy;
    uint16_t total_sessions;
} trainer_summary_t;
void trainer_get_summary(trainer_summary_t *out);

typedef struct {
    uint32_t total_ms; /* summed latency of the correct keystrokes / completed runs */
    uint16_t count;    /* correct keystrokes / completions */
    uint16_t errors;
} trainer_stat_t;

/* Per-character stats: index 0..TRAINER_CHARS-1 is ASCII TRAINER_FIRST_CHAR + index. */
#define TRAINER_CHARS 95
#define TRAINER_FIRST_CHAR 0x20
bool trainer_char_stat(uint8_t index, trainer_stat_t *out);

/* Drill items: built-ins first, then the loaded host items. `*text` stays valid
 * until the host set changes. False if `index` is out of range. */
uint8_t trainer_item_count(void);
bool    trainer_item_stat(uint8_t index, trainer_stat_t *out, uint8_t *group, const char **text);

/* Called from user_hooks.c. trainer_process_record returns false when it consumed the event,
 * trainer_oled_task returns false when it drew the screen. */
bool trainer_process_record(uint16_t keycode, keyrecord_t *record);
void trainer_post_init(void);
void trainer_housekeeping(void);
bool trainer_oled_task(void);

/* Host-supplied drill items; called from host_hid.c. Safe at any time, also mid-session. */
enum trainer_host_result {
    TRAINER_HOST_OK,
    TRAINER_HOST_ERR_FULL,      /* all TRAINER_HOST_ITEMS slots used */
    TRAINER_HOST_ERR_LENGTH,    /* empty or longer than TRAINER_ITEM_MAX_LEN */
    TRAINER_HOST_ERR_CHAR,      /* not printable ASCII, or no key on the current keymap types it */
    TRAINER_HOST_ERR_DUPLICATE, /* already loaded */
};
/* `text` need not be NUL-terminated. On TRAINER_HOST_ERR_CHAR *bad_pos is the offending offset. */
uint8_t trainer_host_add(const char *text, uint8_t len, uint8_t *bad_pos);
void    trainer_host_clear(void);
uint8_t trainer_host_count(void);
/* Drill only host items while any of them is typeable (see trainer.c). */
void trainer_host_set_only(bool only);
bool trainer_host_only(void);
