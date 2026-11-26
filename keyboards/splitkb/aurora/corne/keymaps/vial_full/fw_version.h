/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Firmware version: splash dispatched from user_hooks.c, raw HID from host_hid.c. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Answers a HOST_HID_VERSION report in place (see host_hid.h). */
void fw_version_hid(uint8_t *data, uint8_t length);

/* Returns false while it owns the screen (first seconds after power-up). */
bool fw_version_oled_task(void);
