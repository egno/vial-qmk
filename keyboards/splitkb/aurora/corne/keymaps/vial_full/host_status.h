/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-driven text on the right-half OLED (clock, input source, mic state, ...). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HOST_STATUS_ROWS 3
#define HOST_STATUS_COLS 5 /* width of the rotated OLED */

/* Hooks, dispatched from user_hooks.c. */
void host_status_post_init(void);
void host_status_housekeeping(void);

/* From host_hid.c (master half): `text` is ROWS*COLS chars, `invert` one bit per row;
 * the text expires `ttl_s` seconds later, 0 clears it now. */
void host_status_set(const char *text, uint8_t invert, uint8_t ttl_s);

/* From oled_status.c (secondary half): draws the rows at first_row..first_row+ROWS-1. */
void host_status_render(uint8_t first_row);
