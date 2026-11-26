/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Hold-alone actions; hooks are dispatched from user_hooks.c. */
#pragma once

#include "quantum.h"

/* Last Vial layer; not reachable from the keymap, used only as config. */
#ifndef HOLD_ALONE_LAYER
#    define HOLD_ALONE_LAYER (DYNAMIC_KEYMAP_LAYER_COUNT - 1)
#endif

/* Tracks state only; never consumes the event. */
void hold_alone_process_record(uint16_t keycode, keyrecord_t *record);
void hold_alone_post_process_record(uint16_t keycode, keyrecord_t *record);
