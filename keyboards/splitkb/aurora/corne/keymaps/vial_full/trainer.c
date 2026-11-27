/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * On-board layer/symbol drill for the splitkb Aurora Corne vial_full keymap.
 *
 * - TRAINER_TOGGLE (QK_KB_1) enters/leaves trainer mode.
 * - In trainer mode no keystroke reaches the host. Layer keys still work
 *   (MO, TG, TO, LT when it resolves to hold, layer lock) so layers can be
 *   drilled; everything else is swallowed and scored.
 * - Each key press is resolved to the character it *would* have produced on a
 *   US ANSI host (tap-hold already resolved by QMK's tapping engine, mods
 *   tracked by the trainer itself) and compared with the expected character.
 *
 * Everything except the trainer_* entry points and the QMK user hooks is
 * static. This file owns oled_init_user and
 * rgb_matrix_indicators_advanced_user (which chains to game_display.c while
 * the trainer is off); key processing, post-init,
 * housekeeping and OLED task are shared with the other keymap modules and
 * dispatched from user_hooks.c via trainer_process_record/trainer_post_init/
 * trainer_housekeeping/trainer_oled_task.
 * All of them are pass-through no-ops while the trainer is off.
 *
 * Host-supplied drills: besides the compiled-in table the host can push up to
 * TRAINER_HOST_ITEMS extra strings over raw HID (host_hid.c -> trainer_host_*).
 * They live in RAM only (gone after a power cycle, never written to EEPROM)
 * and form their own group, TG_HOST. Item indices are one space: built-ins
 * first (0..TR_N_DRILLS-1), then the host slots. Mixing rule: host items take
 * part in the same weighted pick with the same per-item stats as built-ins;
 * with "host only" switched on (trainer_host_set_only) the pick is limited to
 * host items for as long as at least one of them is typeable, and silently
 * falls back to everything otherwise, so a session can never stall.
 */
#include QMK_KEYBOARD_H
#include <string.h>
#include "trainer.h"
#include "hold_alone.h"
#include "game_mode.h"
#include "game_display.h"

#if defined(SPLIT_KEYBOARD) && defined(RGB_MATRIX_ENABLE)
#    include "transactions.h"
#    include "split_util.h"
#    define TRAINER_SPLIT_SYNC
#endif

/* ------------------------------------------------------------------------ */
/* Drill tables. Edit freely: plain C strings, printable ASCII only,        */
/* at most TR_ITEM_MAX_LEN characters. Characters are interpreted as what   */
/* a US ANSI host layout would print.                                       */
/* ------------------------------------------------------------------------ */

#define TR_ITEM_MAX_LEN TRAINER_ITEM_MAX_LEN

/* TG_HOST items are not in tr_drills: they are pushed by the host into tr_host_text. */
enum tr_group { TG_PAIRS, TG_SNIPPETS, TG_NUMBERS, TG_HOST, TG_COUNT };

typedef struct {
    uint8_t     group;
    const char *text;
} tr_drill_t;

/* Rejects over-long items at compile time (they would overflow the OLED line buffer). */
#define TR_DRILL(g, s) {(g), (s) + 0 * sizeof(char[(sizeof(s) - 1 <= TR_ITEM_MAX_LEN) ? 1 : -1])}

static const tr_drill_t tr_drills[] = {
    /* (a) bracket / operator pairs */
    TR_DRILL(TG_PAIRS, "()"),
    TR_DRILL(TG_PAIRS, "[]"),
    TR_DRILL(TG_PAIRS, "{}"),
    TR_DRILL(TG_PAIRS, "=>"),
    TR_DRILL(TG_PAIRS, "!="),
    TR_DRILL(TG_PAIRS, "||"),
    TR_DRILL(TG_PAIRS, "->"),
    TR_DRILL(TG_PAIRS, "#["),
    TR_DRILL(TG_PAIRS, "@"),
    TR_DRILL(TG_PAIRS, "=="),
    TR_DRILL(TG_PAIRS, "::"),
    TR_DRILL(TG_PAIRS, "<>"),
    TR_DRILL(TG_PAIRS, "&&"),
    TR_DRILL(TG_PAIRS, "*/"),
    TR_DRILL(TG_PAIRS, "\\n"),
    TR_DRILL(TG_PAIRS, "$_"),
    /* (b) layer-switching snippets */
    TR_DRILL(TG_SNIPPETS, "foo(bar)"),
    TR_DRILL(TG_SNIPPETS, "arr[0]"),
    TR_DRILL(TG_SNIPPETS, "if (x != y) {"),
    TR_DRILL(TG_SNIPPETS, "fn(a, b)"),
    TR_DRILL(TG_SNIPPETS, "map[key] = val"),
    TR_DRILL(TG_SNIPPETS, "x => x * 2"),
    TR_DRILL(TG_SNIPPETS, "p->next"),
    TR_DRILL(TG_SNIPPETS, "#[derive(Debug)]"),
    TR_DRILL(TG_SNIPPETS, "a || !b"),
    TR_DRILL(TG_SNIPPETS, "user@host"),
    TR_DRILL(TG_SNIPPETS, "vec![1, 2, 3]"),
    TR_DRILL(TG_SNIPPETS, "} else {"),
    TR_DRILL(TG_SNIPPETS, "obj{a: 1}"),
    TR_DRILL(TG_SNIPPETS, "x[i] = y[j];"),
    /* (c) numbers: dates, IPs, ports */
    TR_DRILL(TG_NUMBERS, "2025-03-14"),
    TR_DRILL(TG_NUMBERS, "1999-12-31"),
    TR_DRILL(TG_NUMBERS, "31.08.2024"),
    TR_DRILL(TG_NUMBERS, "192.168.1.1"),
    TR_DRILL(TG_NUMBERS, "10.0.0.254"),
    TR_DRILL(TG_NUMBERS, "127.0.0.1:8080"),
    TR_DRILL(TG_NUMBERS, "0.0.0.0:443"),
    TR_DRILL(TG_NUMBERS, "localhost:3000"),
    TR_DRILL(TG_NUMBERS, ":5432"),
    TR_DRILL(TG_NUMBERS, "12:45:07"),
    TR_DRILL(TG_NUMBERS, "v1.20.3"),
    TR_DRILL(TG_NUMBERS, "8.8.4.4"),
};
#define TR_N_DRILLS (sizeof(tr_drills) / sizeof(tr_drills[0]))
#define TR_BUF_LEN (TR_ITEM_MAX_LEN + 8) /* OLED line buffer: "next " + item */
/* Built-ins plus every host slot; stats arrays are sized by this. */
#define TR_ITEMS_MAX (TR_N_DRILLS + TRAINER_HOST_ITEMS)
_Static_assert(TR_ITEMS_MAX < 255, "item index is a uint8_t and 0xff means none");

/* ------------------------------------------------------------------------ */
/* US ANSI host layout: keycode <-> character                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t kc;
    char    plain;
    char    shifted;
} tr_keychar_t;

/* Letters and the number row are handled arithmetically; this is the rest. */
static const tr_keychar_t tr_us_ansi[] = {
    {KC_SPC, ' ', ' '},   {KC_MINS, '-', '_'},  {KC_EQL, '=', '+'},  {KC_LBRC, '[', '{'},
    {KC_RBRC, ']', '}'},  {KC_BSLS, '\\', '|'}, {KC_SCLN, ';', ':'}, {KC_QUOT, '\'', '"'},
    {KC_GRV, '`', '~'},   {KC_COMM, ',', '<'},  {KC_DOT, '.', '>'},  {KC_SLSH, '/', '?'},
    /* Keypad (assumes Num Lock on; shift is ignored by most hosts here). */
    {KC_PSLS, '/', '/'},  {KC_PAST, '*', '*'},  {KC_PMNS, '-', '-'}, {KC_PPLS, '+', '+'},
    {KC_PDOT, '.', '.'},  {KC_PCMM, ',', ','},  {KC_PEQL, '=', '='}, {KC_P0, '0', '0'},
    {KC_P1, '1', '1'},    {KC_P2, '2', '2'},    {KC_P3, '3', '3'},   {KC_P4, '4', '4'},
    {KC_P5, '5', '5'},    {KC_P6, '6', '6'},    {KC_P7, '7', '7'},   {KC_P8, '8', '8'},
    {KC_P9, '9', '9'},
};

static const char tr_num_plain[]   = "1234567890";
static const char tr_num_shifted[] = "!@#$%^&*()";

/* Character a basic keycode produces, or 0 if it isn't a printable char. */
static char tr_kc_to_char(uint8_t kc, bool shift) {
    if (kc >= KC_A && kc <= KC_Z) return (char)((shift ? 'A' : 'a') + (kc - KC_A));
    if (kc >= KC_1 && kc <= KC_0) return shift ? tr_num_shifted[kc - KC_1] : tr_num_plain[kc - KC_1];
    for (uint8_t i = 0; i < sizeof(tr_us_ansi) / sizeof(tr_us_ansi[0]); i++) {
        if (tr_us_ansi[i].kc == kc) return shift ? tr_us_ansi[i].shifted : tr_us_ansi[i].plain;
    }
    return 0;
}

/* Expected char -> (keycode, shifted?) on a US ANSI host. Main-block keys only. */
static bool tr_char_to_key(char c, uint8_t *kc, bool *shift) {
    for (uint8_t k = KC_A; k <= KC_SLSH; k++) {
        for (uint8_t s = 0; s < 2; s++) {
            if (k != KC_NONUS_HASH && tr_kc_to_char(k, s) == c) {
                *kc    = k;
                *shift = s;
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

/* Same values as enum trainer_state, which is exported over raw HID (host_hid.h). */
enum tr_state { TS_OFF = TRAINER_STATE_OFF, TS_RUN = TRAINER_STATE_RUN, TS_RESULTS = TRAINER_STATE_RESULTS, TS_EMPTY = TRAINER_STATE_EMPTY /* nothing typeable on this keymap */ };
#define TR_NONE 0xff /* no item */

enum tr_err {
    TE_WRONG_KEY,     /* plain wrong key */
    TE_MISFIRE_HOLD,  /* tap wanted, key resolved as hold (or a stray mod) */
    TE_MISFIRE_TAP,   /* hold (mod/layer) wanted, key resolved as tap */
    TE_WRONG_LAYER,   /* right physical key, wrong layer active */
    TE_COUNT
};
_Static_assert(TE_COUNT == TRAINER_ERR_KINDS, "exported over raw HID in this order, see host_hid.h");
static const char *const tr_err_name[TE_COUNT] = {"WRONG KEY", "MISFIRE:held", "MISFIRE:tapped", "WRONG LAYER"};

#define TR_NCHARS 95 /* printable ASCII 0x20..0x7e */
_Static_assert(TR_NCHARS == TRAINER_CHARS && TRAINER_FIRST_CHAR == 0x20, "exported over raw HID, see host_hid.h");
#define TR_CIDX(c) ((uint8_t)((c) - 0x20))
#define TR_IS_PRINTABLE(c) ((c) >= 0x20 && (c) <= 0x7e)

typedef struct {
    uint32_t total_ms; /* sum of latencies of correct keystrokes */
    uint16_t count;    /* correct keystrokes */
    uint16_t errors;
} tr_char_stat_t;

typedef struct {
    uint32_t total_ms;
    uint16_t attempts; /* completions */
    uint16_t errors;
} tr_item_stat_t;

/* Where a char lives on the keymap (lowest layer; explicit-shift keys preferred). */
#define TRF_CACHED 0x01
#define TRF_FOUND 0x02
#define TRF_HELD_SHIFT 0x04
typedef struct {
    uint8_t flags, layer, row, col;
} tr_loc_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t magic : 4;
        uint32_t best_wpm : 8;
        uint32_t best_acc : 7;
        uint32_t sessions : 13;
    };
} tr_eeprom_t;
_Static_assert(sizeof(tr_eeprom_t) == sizeof(uint32_t), "EECONFIG_USER is 32 bits");
#define TR_EE_MAGIC 0xA

static struct {
    uint8_t  state;
    uint8_t  cur, next; /* item indices */
    uint8_t  pos;       /* cursor in current item */
    uint32_t last_t;    /* time of last accepted keystroke / item start */
    uint32_t item_ms;
    uint16_t item_errs;

    /* session */
    uint8_t  items_done;
    uint16_t correct, errors;
    uint16_t err_kind[TE_COUNT];
    uint32_t active_ms;
    uint8_t  heat[MATRIX_ROWS][MATRIX_COLS]; /* raw error counts, per expected key */

    /* live feedback */
    uint8_t  last_err; /* 0xff = none */
    uint32_t last_err_t;
    uint32_t results_t; /* when the results screen came up */

    /* mods held through trainer-swallowed keys (mod-tap holds, plain mods) */
    uint8_t key_mods[MATRIX_ROWS][MATRIX_COLS];
    /* a hold that misfired: ignore keystrokes until it is released */
    bool     misfire_active;
    keypos_t misfire_key;

    uint32_t rng;
    uint32_t g_total_ms;
    uint32_t g_count;

    /* host-supplied items (RAM only) */
    uint8_t host_n;
    bool    host_only;
} tr;

static tr_char_stat_t tr_cs[TR_NCHARS];
static tr_item_stat_t tr_is[TR_ITEMS_MAX];
static char           tr_host_text[TRAINER_HOST_ITEMS][TR_ITEM_MAX_LEN + 1];
static tr_loc_t       tr_loc[TR_NCHARS];
static tr_eeprom_t    tr_ee;

/* For the host only (trainer_get_summary): the session counters in `tr` are
 * zeroed when the next session starts, so the finished one is kept here. */
static trainer_summary_t tr_last;
static uint16_t          tr_boot_sessions;

/* RGB state is mirrored to the slave half. */
static struct {
    bool     active;
    bool     show_heat;
    uint8_t  flash_seq;
    uint32_t flash_until;
    uint8_t  heat[MATRIX_ROWS][MATRIX_COLS]; /* scaled 0..255 */
} tr_rgb;

bool trainer_is_active(void) {
    return tr.state != TS_OFF;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static uint32_t tr_rand(void) {
    /* xorshift32 */
    uint32_t x = tr.rng ? tr.rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return tr.rng = x;
}

static uint8_t tr_mods5_to_8(uint8_t m5) {
    return (m5 & 0x10) ? (uint8_t)((m5 & 0x0F) << 4) : (uint8_t)(m5 & 0x0F);
}

static uint8_t tr_layer_count(void) {
#ifdef DYNAMIC_KEYMAP_ENABLE
    return dynamic_keymap_get_layer_count();
#else
    return keymap_layer_count();
#endif
}

static uint8_t tr_held_mods(void) {
    uint8_t m = 0;
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS; c++) m |= tr.key_mods[r][c];
    return m;
}

/* Items currently defined: built-ins plus the loaded host items. */
static uint8_t tr_item_count(void) {
    return (uint8_t)(TR_N_DRILLS + tr.host_n);
}

/* Text of item `i`; "" for anything out of range, so a stale index is harmless. */
static const char *tr_item_text(uint8_t i) {
    if (i < TR_N_DRILLS) return tr_drills[i].text;
    if (i < tr_item_count()) return tr_host_text[i - TR_N_DRILLS];
    return "";
}

static char tr_expected(void) {
    return tr_item_text(tr.cur)[tr.pos];
}

/*
 * Character the keymap key at (layer, key) produces when *tapped*, given
 * extra held mods. Transparent keys return 0 (caller decides). Non-chars 0.
 */
static char tr_key_char(uint8_t layer, keypos_t key, uint8_t held) {
    if (layer == HOLD_ALONE_LAYER) return 0; /* config only, never active */
    /* Game layers repeat the alphabet in QWERTY: they must not own characters
     * (tr_locate), nor turn a miss into WRONG LAYER / a layer-tap misfire. */
    if (layer >= GAME_LAYER) return 0;
    uint16_t kc   = keymap_key_to_keycode(layer, key);
    uint8_t  mods = held;
    uint8_t  base;
    if (IS_QK_MOD_TAP(kc)) {
        base = QK_MOD_TAP_GET_TAP_KEYCODE(kc);
    } else if (IS_QK_LAYER_TAP(kc)) {
        base = QK_LAYER_TAP_GET_TAP_KEYCODE(kc);
    } else if (IS_QK_MODS(kc)) {
        base = QK_MODS_GET_BASIC_KEYCODE(kc);
        mods |= tr_mods5_to_8(QK_MODS_GET_MODS(kc));
    } else if (IS_BASIC_KEYCODE(kc)) {
        base = (uint8_t)kc;
    } else {
        return 0;
    }
    if (mods & ~MOD_MASK_SHIFT) return 0;
    return tr_kc_to_char(base, mods & MOD_MASK_SHIFT);
}

/* Find the physical key for a char: lowest layer, explicit keycodes first. */
static const tr_loc_t *tr_locate(char ch) {
    tr_loc_t *l = &tr_loc[TR_CIDX(ch)];
    if (l->flags & TRF_CACHED) return l;
    l->flags          = TRF_CACHED;
    uint8_t layers    = tr_layer_count();
    for (uint8_t pass = 0; pass < 2; pass++) {
        uint8_t held = pass ? MOD_BIT(KC_LSFT) : 0;
        for (uint8_t ly = 0; ly < layers; ly++) {
            for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
                for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                    keypos_t k = {.row = r, .col = c};
                    if (tr_key_char(ly, k, held) == ch) {
                        l->flags |= TRF_FOUND | (pass ? TRF_HELD_SHIFT : 0);
                        l->layer = ly;
                        l->row   = r;
                        l->col   = c;
                        return l;
                    }
                }
            }
        }
    }
    return l;
}

static bool tr_layer_has_char(uint8_t layer, char ch) {
    if (layer >= tr_layer_count()) return false;
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            keypos_t k = {.row = r, .col = c};
            if (tr_key_char(layer, k, 0) == ch || tr_key_char(layer, k, MOD_BIT(KC_LSFT)) == ch) return true;
        }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Adaptive item selection                                                  */
/* ------------------------------------------------------------------------ */

/* Difficulty of a char: 100 = average speed, no errors. */
static uint32_t tr_char_score(char ch) {
    const tr_char_stat_t *s = &tr_cs[TR_CIDX(ch)];
    if (s->count == 0) return s->errors ? 400 : 160; /* never typed correctly yet */
    uint32_t avg   = s->total_ms / s->count;
    uint32_t g_avg = tr.g_count ? (tr.g_total_ms / tr.g_count) : avg;
    if (g_avg == 0) g_avg = 1;
    return avg * 100 / g_avg + (uint32_t)s->errors * 300 / (s->count + s->errors);
}

/* 0 for items the current keymap cannot type (e.g. after a Vial remap):
 * they would stall the session, so they are never picked. */
static uint32_t tr_item_weight(uint8_t i) {
    uint32_t    w = 20;
    const char *t = tr_item_text(i);
    if (!*t) return 0;
    for (; *t; t++) {
        if (!TR_IS_PRINTABLE(*t) || !(tr_locate(*t)->flags & TRF_FOUND)) return 0;
        uint32_t s = tr_char_score(*t);
        if (s > 80) w += s - 80;
    }
    if (tr_is[i].attempts == 0) w += 150;
    return w;
}

/* First index the pick may use: the host group in "host only" mode while it
 * has anything typeable, otherwise everything. */
static uint8_t tr_pick_from(void) {
    if (tr.host_only) {
        for (uint8_t i = TR_N_DRILLS; i < tr_item_count(); i++)
            if (tr_item_weight(i)) return TR_N_DRILLS;
    }
    return 0;
}

static uint8_t tr_pick(uint8_t avoid_a, uint8_t avoid_b) {
    uint8_t  from = tr_pick_from(), n = tr_item_count();
    uint32_t total = 0;
    for (uint8_t i = from; i < n; i++)
        if (i != avoid_a && i != avoid_b) total += tr_item_weight(i);
    if (total == 0) {
        /* Only the avoided items are typeable: repeat one rather than stall. */
        if (avoid_a >= from && avoid_a < n && tr_item_weight(avoid_a)) return avoid_a;
        if (avoid_b >= from && avoid_b < n && tr_item_weight(avoid_b)) return avoid_b;
        return TR_NONE;
    }
    uint32_t r = tr_rand() % total;
    for (uint8_t i = from; i < n; i++) {
        if (i == avoid_a || i == avoid_b) continue;
        uint32_t w = tr_item_weight(i);
        if (r < w) return i;
        r -= w;
    }
    return TR_NONE;
}

/* ------------------------------------------------------------------------ */
/* Session flow                                                             */
/* ------------------------------------------------------------------------ */

static void tr_flash(void) {
    tr_rgb.flash_seq++;
    tr_rgb.flash_until = timer_read32() + TRAINER_FLASH_MS;
}

static void tr_start_item(uint8_t idx) {
    tr.cur       = idx;
    tr.pos       = 0;
    tr.item_ms   = 0;
    tr.item_errs = 0;
    tr.last_t    = timer_read32();
    tr.next      = tr_pick(tr.cur, TR_NONE);
}

static void tr_start_session(void) {
    tr.state      = TS_RUN;
    tr.items_done = 0;
    tr.correct    = 0;
    tr.errors     = 0;
    tr.active_ms  = 0;
    tr.last_err   = 0xff;
    memset(tr.err_kind, 0, sizeof(tr.err_kind));
    memset(tr.heat, 0, sizeof(tr.heat));
    tr_rgb.show_heat = false;
    tr.rng ^= timer_read32();
    uint8_t first = tr_pick(TR_NONE, TR_NONE);
    if (first == TR_NONE) {
        tr.state = TS_EMPTY;
        tr.cur = tr.next = TR_NONE;
        return;
    }
    tr_start_item(first);
}

static uint8_t tr_session_wpm(void) {
    if (tr.active_ms == 0) return 0;
    uint32_t w = (uint32_t)tr.correct * 12000u / tr.active_ms; /* (chars/5) per minute */
    return w > 255 ? 255 : (uint8_t)w;
}

static uint8_t tr_session_acc(void) {
    uint32_t n = (uint32_t)tr.correct + tr.errors;
    return n ? (uint8_t)((uint32_t)tr.correct * 100u / n) : 100;
}

static void tr_finish_session(void) {
    tr.state     = TS_RESULTS;
    tr.results_t = timer_read32();

    uint8_t wpm = tr_session_wpm(), acc = tr_session_acc();
    if (wpm > tr_ee.best_wpm) tr_ee.best_wpm = wpm;
    if (acc > tr_ee.best_acc) tr_ee.best_acc = acc;
    if (tr_ee.sessions < 0x1FFF) tr_ee.sessions++;
    tr_ee.magic = TR_EE_MAGIC;
    eeconfig_update_user(tr_ee.raw);

    tr_last.wpm       = wpm;
    tr_last.accuracy  = acc;
    tr_last.items     = tr.items_done;
    tr_last.correct   = tr.correct;
    tr_last.errors    = tr.errors;
    tr_last.active_ms = tr.active_ms;
    memcpy(tr_last.err_kind, tr.err_kind, sizeof(tr_last.err_kind));
    tr_boot_sessions++;

    uint8_t mx = 0;
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS; c++)
            if (tr.heat[r][c] > mx) mx = tr.heat[r][c];
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS; c++)
            tr_rgb.heat[r][c] = mx ? (uint8_t)((uint16_t)tr.heat[r][c] * 255u / mx) : 0;
    tr_rgb.show_heat = true;
}

static void tr_record_error(uint8_t kind) {
    char ch = tr_expected();
    tr.errors++;
    tr.err_kind[kind]++;
    tr.item_errs++;
    tr.last_err   = kind;
    tr.last_err_t = timer_read32();
    if (TR_IS_PRINTABLE(ch)) {
        tr_char_stat_t *s = &tr_cs[TR_CIDX(ch)];
        if (s->errors < UINT16_MAX) s->errors++;
        const tr_loc_t *l = tr_locate(ch);
        if ((l->flags & TRF_FOUND) && tr.heat[l->row][l->col] < 255) tr.heat[l->row][l->col]++;
    }
    tr_flash();
}

static void tr_accept(void) {
    uint32_t now = timer_read32();
    uint32_t lat = TIMER_DIFF_32(now, tr.last_t);
    if (lat > TRAINER_LATENCY_CAP_MS) lat = TRAINER_LATENCY_CAP_MS;
    tr.last_t = now;

    char ch = tr_expected();
    if (TR_IS_PRINTABLE(ch)) {
        tr_char_stat_t *s = &tr_cs[TR_CIDX(ch)];
        if (s->count < UINT16_MAX) {
            s->count++;
            s->total_ms += lat;
        }
    }
    tr.g_total_ms += lat;
    tr.g_count++;
    tr.correct++;
    tr.active_ms += lat;
    tr.item_ms += lat;
    tr.rng ^= now;

    if (tr.cur >= tr_item_count()) return; /* cannot happen in TS_RUN; keeps tr_is in bounds */
    tr.pos++;
    if (tr_item_text(tr.cur)[tr.pos] == '\0') {
        tr_item_stat_t *is = &tr_is[tr.cur];
        if (is->attempts < UINT16_MAX) is->attempts++;
        is->total_ms += tr.item_ms;
        is->errors += tr.item_errs;
        if (++tr.items_done >= TRAINER_SESSION_ITEMS) {
            tr_finish_session();
        } else {
            uint8_t nxt = tr.next;
            uint8_t prv = tr.cur;
            tr_start_item(nxt);
            tr.next = tr_pick(tr.cur, prv);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Judging                                                                  */
/* ------------------------------------------------------------------------ */

enum tr_src { SRC_PLAIN, SRC_MT_TAP, SRC_LT_TAP };

/*
 * A key resolved as a tap (or was a plain key). `kc` is the basic keycode it
 * would send, `explicit_mods` any mods baked into the keycode (LSFT(...)),
 * `src_arg` the mod-tap's mods (SRC_MT_TAP) or the layer-tap's layer.
 */
static void tr_judge(uint8_t kc, uint8_t explicit_mods, keypos_t key, uint8_t src, uint8_t src_arg) {
    if (tr.state == TS_EMPTY) return;
    if (tr.state == TS_RESULTS) {
        /* Any tapped key starts the next round, once the screen had a chance to be read. */
        if (timer_elapsed32(tr.results_t) >= TRAINER_RESULTS_HOLD_MS) tr_start_session();
        return;
    }
    if (tr.misfire_active) return; /* already penalised; don't double count */

    uint8_t held = tr_held_mods();
    uint8_t mods = held | explicit_mods;

    if (kc == KC_BSPC && !mods) return; /* reflexive backspace: ignored */

    char produced = (mods & ~MOD_MASK_SHIFT) ? 0 : tr_kc_to_char(kc, mods & MOD_MASK_SHIFT);
    char expected = tr_expected();

    if (produced == expected) {
        tr_accept();
        return;
    }

    /* --- classify the miss --- */
    uint8_t         kind = TE_WRONG_KEY;
    const tr_loc_t *loc  = tr_locate(expected);

    if (held & ~MOD_MASK_SHIFT) {
        /* A ctrl/alt/gui hold leaked into this keystroke: home-row mod misfire. */
        kind = TE_MISFIRE_HOLD;
    } else if (src == SRC_MT_TAP && (src_arg & MOD_MASK_SHIFT) && (loc->flags & TRF_HELD_SHIFT)) {
        /* Tapped a shift mod-tap while the target needs a held shift. */
        kind = TE_MISFIRE_TAP;
    } else if (src == SRC_LT_TAP && tr_layer_has_char(src_arg, expected)) {
        /* Tapped a layer-tap whose layer holds the target. */
        kind = TE_MISFIRE_TAP;
    } else {
        /* Same physical key gives the target on another layer? */
        uint8_t layers = tr_layer_count();
        uint8_t hshift = held & MOD_MASK_SHIFT;
        for (uint8_t ly = 0; ly < layers; ly++) {
            if (tr_key_char(ly, key, hshift) == expected) {
                kind = TE_WRONG_LAYER;
                break;
            }
        }
        uint8_t exp_kc;
        bool    exp_shift;
        if (kind == TE_WRONG_KEY && produced && tr_char_to_key(expected, &exp_kc, &exp_shift) && exp_kc == kc) {
            /* Right key, wrong shift state => shift mod-tap misfire:
             * shift held when it shouldn't be (hold), or not engaged (tap). */
            kind = (mods & MOD_MASK_SHIFT) ? TE_MISFIRE_HOLD : TE_MISFIRE_TAP;
        }
    }
    tr_record_error(kind);
}

/*
 * A tap-hold key resolved as HOLD. If its tap would have produced the
 * expected char, the user wanted the tap: misfire.
 */
static void tr_check_hold(uint8_t tap_kc, keypos_t key) {
    if (tr.state != TS_RUN || tr.misfire_active) return;
    char expected = tr_expected();
    if (tr_kc_to_char(tap_kc, false) == expected || tr_kc_to_char(tap_kc, true) == expected) {
        tr_record_error(TE_MISFIRE_HOLD);
        tr.misfire_active = true;
        tr.misfire_key    = key;
    }
}

static void tr_release_hold(keypos_t key) {
    if (tr.misfire_active && tr.misfire_key.row == key.row && tr.misfire_key.col == key.col) tr.misfire_active = false;
}

static bool tr_is_layer_keycode(uint16_t kc) {
    return IS_QK_MOMENTARY(kc) || IS_QK_TO(kc) || IS_QK_TOGGLE_LAYER(kc) || IS_QK_ONE_SHOT_LAYER(kc) || IS_QK_LAYER_TAP_TOGGLE(kc) || IS_QK_DEF_LAYER(kc) || IS_QK_PERSISTENT_DEF_LAYER(kc) || IS_QK_LAYER_MOD(kc) || kc == QK_LAYER_LOCK;
}

/* Returns whether QMK should keep processing the event (only layer actions). */
static bool tr_process(uint16_t keycode, keyrecord_t *record) {
    if (!IS_KEYEVENT(record->event)) return false; /* encoders, combos: swallowed */

    keypos_t key     = record->event.key;
    bool     pressed = record->event.pressed;
    if (key.row >= MATRIX_ROWS || key.col >= MATRIX_COLS) return false;

    if (tr_is_layer_keycode(keycode)) return true;

    if (IS_QK_LAYER_TAP(keycode)) {
        if (record->tap.count == 0) { /* hold: let the layer switch happen */
            if (pressed) {
                tr_check_hold(QK_LAYER_TAP_GET_TAP_KEYCODE(keycode), key);
            } else {
                tr_release_hold(key);
            }
            return true;
        }
        if (pressed) tr_judge(QK_LAYER_TAP_GET_TAP_KEYCODE(keycode), 0, key, SRC_LT_TAP, QK_LAYER_TAP_GET_LAYER(keycode));
        return false;
    }

    if (IS_QK_MOD_TAP(keycode)) {
        uint8_t m = tr_mods5_to_8(QK_MOD_TAP_GET_MODS(keycode));
        if (record->tap.count == 0) { /* hold: track the mod ourselves, send nothing */
            if (pressed) {
                tr_check_hold(QK_MOD_TAP_GET_TAP_KEYCODE(keycode), key);
                tr.key_mods[key.row][key.col] = m;
            } else {
                tr.key_mods[key.row][key.col] = 0;
                tr_release_hold(key);
            }
            return false;
        }
        if (pressed) tr_judge(QK_MOD_TAP_GET_TAP_KEYCODE(keycode), 0, key, SRC_MT_TAP, m);
        return false;
    }

    if (IS_MODIFIER_KEYCODE(keycode)) {
        tr.key_mods[key.row][key.col] = pressed ? MOD_BIT(keycode) : 0;
        return false;
    }

    if (!pressed) return false;

    if (IS_QK_MODS(keycode)) {
        tr_judge(QK_MODS_GET_BASIC_KEYCODE(keycode), tr_mods5_to_8(QK_MODS_GET_MODS(keycode)), key, SRC_PLAIN, 0);
    } else if (IS_BASIC_KEYCODE(keycode)) {
        tr_judge((uint8_t)keycode, 0, key, SRC_PLAIN, 0);
    } else {
        tr_judge(KC_NO, 0, key, SRC_PLAIN, 0); /* mouse/RGB/macro/...: never a char */
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Enter / leave                                                            */
/* ------------------------------------------------------------------------ */

void trainer_toggle(void) {
    if (tr.state == TS_OFF) {
        clear_keyboard(); /* release anything the host currently sees held */
        memset(tr.key_mods, 0, sizeof(tr.key_mods));
        memset(tr_loc, 0, sizeof(tr_loc)); /* keymap may have changed via Vial */
        tr.misfire_active = false;
        tr.rng ^= timer_read32() * 2654435761u;
        tr_rgb.active = true;
        tr_start_session();
    } else {
        tr.state         = TS_OFF;
        tr_rgb.active    = false;
        tr_rgb.show_heat = false;
        memset(tr.key_mods, 0, sizeof(tr.key_mods));
        tr.misfire_active = false;
    }
#ifdef OLED_ENABLE
    if (is_keyboard_master()) oled_init(OLED_ROTATION_0); /* re-run rotation hooks */
#endif
}

/* ------------------------------------------------------------------------ */
/* Host-supplied items (raw HID, see host_hid.h)                            */
/* ------------------------------------------------------------------------ */

/* The item set or the pick range changed under a possibly running session.
 * Raw HID is handled in the main loop like key events, so this never
 * interleaves with tr_judge; it only has to leave cur/next valid. */
static void tr_host_changed(void) {
    if (tr.state == TS_EMPTY) {
        tr_start_session(); /* maybe something is typeable now */
    } else if (tr.state == TS_RUN) {
        if (tr.cur >= tr_item_count()) {
            /* The item being typed is gone: drop it uncounted and move on. */
            uint8_t first = tr_pick(TR_NONE, TR_NONE);
            if (first == TR_NONE) {
                tr.state = TS_EMPTY;
                tr.cur = tr.next = TR_NONE;
            } else {
                tr_start_item(first);
            }
        } else {
            tr.next = tr_pick(tr.cur, TR_NONE);
        }
    }
}

uint8_t trainer_host_count(void) {
    return tr.host_n;
}

bool trainer_host_only(void) {
    return tr.host_only;
}

void trainer_host_set_only(bool only) {
    if (tr.host_only == only) return;
    tr.host_only = only;
    tr_host_changed();
}

void trainer_host_clear(void) {
    tr.host_n = 0;
    memset(tr_host_text, 0, sizeof(tr_host_text));
    memset(&tr_is[TR_N_DRILLS], 0, sizeof(tr_is[0]) * TRAINER_HOST_ITEMS);
    tr_host_changed();
}

uint8_t trainer_host_add(const char *text, uint8_t len, uint8_t *bad_pos) {
    *bad_pos = 0;
    if (len == 0 || len > TR_ITEM_MAX_LEN) return TRAINER_HOST_ERR_LENGTH;
    /* While the trainer is off the location cache may predate a Vial remap. */
    if (tr.state == TS_OFF) memset(tr_loc, 0, sizeof(tr_loc));
    for (uint8_t i = 0; i < len; i++) {
        if (!TR_IS_PRINTABLE(text[i]) || !(tr_locate(text[i])->flags & TRF_FOUND)) {
            *bad_pos = i;
            return TRAINER_HOST_ERR_CHAR;
        }
    }
    for (uint8_t i = 0; i < tr.host_n; i++) {
        if (strlen(tr_host_text[i]) == len && memcmp(tr_host_text[i], text, len) == 0) return TRAINER_HOST_ERR_DUPLICATE;
    }
    if (tr.host_n >= TRAINER_HOST_ITEMS) return TRAINER_HOST_ERR_FULL;
    /* The slot's stats were zeroed by the last clear (or never used). */
    memcpy(tr_host_text[tr.host_n], text, len);
    tr_host_text[tr.host_n][len] = '\0';
    tr.host_n++;
    tr_host_changed();
    return TRAINER_HOST_OK;
}

/* ------------------------------------------------------------------------ */
/* Read-only exports for the host (raw HID, see host_hid.h)                 */
/* ------------------------------------------------------------------------ */

uint8_t trainer_state(void) {
    return tr.state;
}

uint16_t trainer_boot_sessions(void) {
    return tr_boot_sessions;
}

void trainer_get_summary(trainer_summary_t *out) {
    *out                = tr_last;
    out->best_wpm       = tr_ee.best_wpm;
    out->best_accuracy  = tr_ee.best_acc;
    out->total_sessions = tr_ee.sessions;
}

bool trainer_char_stat(uint8_t index, trainer_stat_t *out) {
    if (index >= TR_NCHARS) return false;
    *out = (trainer_stat_t){.total_ms = tr_cs[index].total_ms, .count = tr_cs[index].count, .errors = tr_cs[index].errors};
    return true;
}

uint8_t trainer_item_count(void) {
    return tr_item_count();
}

bool trainer_item_stat(uint8_t index, trainer_stat_t *out, uint8_t *group, const char **text) {
    if (index >= tr_item_count()) return false;
    *out   = (trainer_stat_t){.total_ms = tr_is[index].total_ms, .count = tr_is[index].attempts, .errors = tr_is[index].errors};
    *group = index < TR_N_DRILLS ? tr_drills[index].group : TG_HOST;
    *text  = tr_item_text(index);
    return true;
}

/* ------------------------------------------------------------------------ */
/* QMK hooks                                                                */
/* ------------------------------------------------------------------------ */

bool trainer_process_record(uint16_t keycode, keyrecord_t *record) {
    if (keycode == TRAINER_TOGGLE) {
        if (record->event.pressed) trainer_toggle();
        return false;
    }
    if (tr.state == TS_OFF) return true;
    return tr_process(keycode, record);
}

#ifdef TRAINER_SPLIT_SYNC
typedef struct {
    uint8_t active;
    uint8_t show_heat;
    uint8_t flash_seq;
    uint8_t heat[MATRIX_ROWS / 2][MATRIX_COLS];
} tr_sync_t;
_Static_assert(sizeof(tr_sync_t) <= RPC_M2S_BUFFER_SIZE, "trainer sync message too big for split RPC");

static void tr_sync_slave(uint8_t in_len, const void *in_data, uint8_t out_len, void *out_data) {
    if (in_len != sizeof(tr_sync_t)) return;
    const tr_sync_t *m   = (const tr_sync_t *)in_data;
    uint8_t          off = is_keyboard_left() ? 0 : MATRIX_ROWS / 2;
    if (m->flash_seq != tr_rgb.flash_seq) {
        tr_rgb.flash_seq   = m->flash_seq;
        tr_rgb.flash_until = timer_read32() + TRAINER_FLASH_MS;
    }
    tr_rgb.active    = m->active;
    tr_rgb.show_heat = m->show_heat;
    memcpy(&tr_rgb.heat[off][0], m->heat, sizeof(m->heat));
}

static void tr_sync_master(void) {
    static tr_sync_t last;
    static uint32_t  last_t, last_try;
    if (!is_keyboard_master() || !is_transport_connected()) return;
    if (timer_elapsed32(last_try) < 20) return; /* don't hammer the link on failure */
    tr_sync_t m;
    uint8_t   off = is_keyboard_left() ? MATRIX_ROWS / 2 : 0; /* the other half's rows */
    m.active      = tr_rgb.active;
    m.show_heat   = tr_rgb.show_heat;
    m.flash_seq   = tr_rgb.flash_seq;
    memcpy(m.heat, &tr_rgb.heat[off][0], sizeof(m.heat));
    bool changed = memcmp(&m, &last, sizeof(m)) != 0;
    if (changed || (m.active && timer_elapsed32(last_t) > 1000)) {
        last_try = timer_read32();
        if (transaction_rpc_send(TRAINER_SYNC, sizeof(m), &m)) {
            last   = m;
            last_t = timer_read32();
        }
    }
}
#else
static void tr_sync_master(void) {}
#endif

void trainer_housekeeping(void) {
#if TRAINER_IDLE_EXIT_MS > 0
    /* Only the master ever has the trainer on. Forgotten drills must not leave
     * the keyboard mute: hand the keys back to the host once it sits idle. */
    if (tr.state != TS_OFF && last_input_activity_elapsed() > TRAINER_IDLE_EXIT_MS) trainer_toggle();
#endif
    tr_sync_master();
}

void trainer_post_init(void) {
    tr_ee.raw = eeconfig_read_user();
    if (tr_ee.magic != TR_EE_MAGIC) tr_ee.raw = 0;
    tr.last_err = 0xff;
#ifdef TRAINER_SPLIT_SYNC
    transaction_register_rpc(TRAINER_SYNC, tr_sync_slave);
#endif
}

#ifdef RGB_MATRIX_ENABLE
bool rgb_matrix_indicators_advanced_user(uint8_t led_min, uint8_t led_max) {
    /* The trainer's display wins while it is active; otherwise game mode gets the LEDs. */
    if (!tr_rgb.active) return game_rgb_indicators(led_min, led_max);
    uint8_t val = rgb_matrix_get_val();
    if (val < 48) val = 48;

    if (tr_rgb.flash_until && !timer_expired32(timer_read32(), tr_rgb.flash_until)) {
        for (uint8_t i = led_min; i < led_max; i++) rgb_matrix_set_color(i, val, 0, 0);
        return false;
    }
    if (tr_rgb.show_heat) {
        for (uint8_t i = led_min; i < led_max; i++) rgb_matrix_set_color(i, 0, 0, 0);
        for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                uint8_t idx = g_led_config.matrix_co[r][c];
                uint8_t lvl = tr_rgb.heat[r][c];
                if (idx == NO_LED) continue;
                rgb_t rgb;
                if (lvl == 0) {
                    hsv_t hsv = {85, 255, val / 4}; /* dim green: no misses */
                    rgb       = hsv_to_rgb(hsv);
                } else {
                    hsv_t hsv = {(uint8_t)(43 - (uint16_t)lvl * 43 / 255), 255, val}; /* yellow -> red */
                    rgb       = hsv_to_rgb(hsv);
                }
                RGB_MATRIX_INDICATOR_SET_COLOR(idx, rgb.r, rgb.g, rgb.b);
            }
        }
        return false;
    }
    return true;
}
#endif

#ifdef OLED_ENABLE
oled_rotation_t oled_init_user(oled_rotation_t rotation) {
    /* Keyboard default is portrait (270); the trainer wants 128x32 landscape. */
    return tr.state != TS_OFF ? OLED_ROTATION_0 : rotation;
}

static char *tr_utoa(char *p, uint32_t v) {
    char     tmp[10];
    uint8_t  n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof(tmp));
    while (n) *p++ = tmp[--n];
    return p;
}

static char *tr_puts(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

/* Write `s` padded/truncated to a full line. */
static void tr_line(uint8_t row, const char *s) {
    uint8_t w = oled_max_chars();
    oled_set_cursor(0, row);
    for (uint8_t i = 0; i < w; i++) {
        char ch = *s ? *s++ : ' ';
        oled_write_char(ch, false);
    }
}

static void tr_render_run(void) {
    uint8_t     w    = oled_max_chars();
    const char *t    = tr_item_text(tr.cur);
    uint8_t     len  = (uint8_t)strlen(t);
    uint8_t     from = 0;
    if (len > w && tr.pos > w / 2) from = tr.pos - w / 2;
    if (len > w && from > len - w) from = len - w;

    /* Row 0: target. Row 1: a caret under the character to type next. */
    oled_set_cursor(0, 0);
    for (uint8_t i = 0; i < w; i++) {
        uint8_t j = from + i;
        oled_write_char(j < len ? t[j] : ' ', false);
    }
    oled_set_cursor(0, 1);
    for (uint8_t i = 0; i < w; i++) {
        oled_write_char(from + i == tr.pos ? '^' : ' ', false);
    }

    char buf[TR_BUF_LEN], *p;

    /* Row 2: live accuracy / speed / progress. */
    p = tr_utoa(buf, tr_session_acc());
    p = tr_puts(p, "% ");
    p = tr_utoa(p, tr_session_wpm());
    p = tr_puts(p, "wpm ");
    p = tr_utoa(p, tr.items_done + 1);
    *p++ = '/';
    p = tr_utoa(p, TRAINER_SESSION_ITEMS);
    *p = 0;
    tr_line(2, buf);

    /* Row 3: last miss for 1.5s, otherwise layer + error count. */
    if (tr.last_err < TE_COUNT && timer_elapsed32(tr.last_err_t) < 1500) {
        p = tr_puts(buf, "x ");
        p = tr_puts(p, tr_err_name[tr.last_err]);
    } else {
        p = tr_puts(buf, "L");
        p = tr_utoa(p, get_highest_layer(layer_state | default_layer_state));
        p = tr_puts(p, " err ");
        p = tr_utoa(p, tr.errors);
        if (tr.misfire_active) p = tr_puts(p, " release!");
    }
    *p = 0;
    tr_line(3, buf);
}

static void tr_render_results(void) {
    char buf[TR_BUF_LEN], *p;

    p = tr_puts(buf, "DONE ");
    p = tr_utoa(p, tr_session_wpm());
    p = tr_puts(p, "wpm ");
    p = tr_utoa(p, tr_session_acc());
    p = tr_puts(p, "%");
    *p = 0;
    tr_line(0, buf);

    p = tr_puts(buf, "best ");
    p = tr_utoa(p, tr_ee.best_wpm);
    p = tr_puts(p, "wpm ");
    p = tr_utoa(p, tr_ee.best_acc);
    p = tr_puts(p, "% ");
    p = tr_utoa(p, tr_ee.sessions);
    *p++ = 'x';
    *p = 0;
    tr_line(1, buf);

    /* Error breakdown: wrong key / misfire hold / misfire tap / wrong layer. */
    p = tr_puts(buf, "K");
    p = tr_utoa(p, tr.err_kind[TE_WRONG_KEY]);
    p = tr_puts(p, " H");
    p = tr_utoa(p, tr.err_kind[TE_MISFIRE_HOLD]);
    p = tr_puts(p, " T");
    p = tr_utoa(p, tr.err_kind[TE_MISFIRE_TAP]);
    p = tr_puts(p, " L");
    p = tr_utoa(p, tr.err_kind[TE_WRONG_LAYER]);
    *p = 0;
    tr_line(2, buf);

    /* Three hardest chars overall (cumulative since power-on). */
    p = tr_puts(buf, "drill:");
    uint8_t picked[3] = {0xff, 0xff, 0xff};
    for (uint8_t k = 0; k < 3; k++) {
        uint32_t best = 0;
        for (uint8_t i = 1; i < TR_NCHARS; i++) { /* skip space */
            if (i == picked[0] || i == picked[1]) continue;
            if (tr_cs[i].count == 0 && tr_cs[i].errors == 0) continue;
            uint32_t s = tr_char_score((char)(i + 0x20));
            if (s > best) {
                best      = s;
                picked[k] = i;
            }
        }
        if (picked[k] != 0xff) {
            *p++ = ' ';
            *p++ = (char)(picked[k] + 0x20);
        }
    }
    *p = 0;
    tr_line(3, buf);
}

bool trainer_oled_task(void) {
    if (tr.state == TS_OFF) return true;
    if (!is_keyboard_master()) return true;
    if (tr.state == TS_RUN) {
        tr_render_run();
    } else if (tr.state == TS_RESULTS) {
        tr_render_results();
    } else {
        tr_line(0, "TRAINER");
        tr_line(1, "no drill is typeable");
        tr_line(2, "on this keymap");
        tr_line(3, "");
    }
    return false;
}
#endif
