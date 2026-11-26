/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-driven status text for the right-half OLED.
 *
 * The host sets HOST_STATUS_ROWS x HOST_STATUS_COLS characters over raw HID
 * (HOST_HID_STATUS in host_hid.h). USB ends on the master half, the screen
 * hangs off the secondary one, so the master forwards the text over its own
 * split transaction (HOST_STATUS_SYNC): on change, and once a second so a
 * re-plugged secondary catches up. The master also owns the time-to-live:
 * when the host stops refreshing, the text is blanked and the blank is synced.
 */
#include "quantum.h"
#include "transactions.h"
#include "split_util.h"
#include "host_status.h"
#include <string.h>

typedef struct {
    uint8_t invert; /* bit n: row n inverted */
    char    text[HOST_STATUS_ROWS * HOST_STATUS_COLS];
} host_status_t;
_Static_assert(sizeof(host_status_t) <= RPC_M2S_BUFFER_SIZE, "host status message too big for split RPC");

static host_status_t hs;
static uint32_t      hs_set_at, hs_ttl_ms; /* master: 0 ttl = nothing shown */

static void hs_blank(void) {
    hs.invert = 0;
    memset(hs.text, ' ', sizeof(hs.text));
}

static void hs_slave_handler(uint8_t in_len, const void *in_data, uint8_t out_len, void *out_data) {
    if (in_len == sizeof(hs)) memcpy(&hs, in_data, sizeof(hs));
}

void host_status_post_init(void) {
    hs_blank();
    transaction_register_rpc(HOST_STATUS_SYNC, hs_slave_handler);
}

void host_status_set(const char *text, uint8_t invert, uint8_t ttl_s) {
    hs_blank();
    hs_ttl_ms = (uint32_t)ttl_s * 1000;
    hs_set_at = timer_read32();
    if (ttl_s == 0) return;
    hs.invert = invert & ((1 << HOST_STATUS_ROWS) - 1);
    for (uint8_t i = 0; i < sizeof(hs.text); i++) {
        if (text[i] > 0x20 && text[i] < 0x7f) hs.text[i] = text[i];
    }
}

void host_status_housekeeping(void) {
    if (!is_keyboard_master()) return;
    if (hs_ttl_ms && timer_elapsed32(hs_set_at) > hs_ttl_ms) {
        hs_ttl_ms = 0;
        hs_blank();
    }
    static host_status_t last_sent;
    static uint32_t      last_sync, last_try;
    if (!is_transport_connected() || timer_elapsed32(last_try) < 20) return; /* don't hammer a dead link */
    if (memcmp(&hs, &last_sent, sizeof(hs)) != 0 || timer_elapsed32(last_sync) > 1000) {
        last_try = timer_read32();
        if (transaction_rpc_send(HOST_STATUS_SYNC, sizeof(hs), &hs)) {
            last_sent = hs;
            last_sync = timer_read32();
        }
    }
}

#ifdef OLED_ENABLE
void host_status_render(uint8_t first_row) {
    for (uint8_t r = 0; r < HOST_STATUS_ROWS; r++) {
        const char *row   = &hs.text[r * HOST_STATUS_COLS];
        bool        blank = memcmp(row, "     ", HOST_STATUS_COLS) == 0;
        oled_set_cursor(0, first_row + r);
        for (uint8_t c = 0; c < HOST_STATUS_COLS; c++) {
            oled_write_char(row[c], !blank && (hs.invert & (1 << r)));
        }
    }
}
#else
void host_status_render(uint8_t first_row) {}
#endif
