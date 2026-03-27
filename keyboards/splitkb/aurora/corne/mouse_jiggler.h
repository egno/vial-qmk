/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "action.h"

#ifndef MOUSE_JIGGLER_IDLE_MS
#    define MOUSE_JIGGLER_IDLE_MS 60000
#endif
#ifndef MOUSE_JIGGLER_INTERVAL_MS
#    define MOUSE_JIGGLER_INTERVAL_MS 25000
#endif

void mouse_jiggler_post_init_kb(void);
void mouse_jiggler_housekeeping_kb(void);
bool mouse_jiggler_process_record_kb(uint16_t keycode, keyrecord_t *record);

bool mouse_jiggler_is_feature_enabled(void);
