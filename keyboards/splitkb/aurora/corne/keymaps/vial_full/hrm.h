/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Home-row mod tuning (Chordal Hold + Flow Tap); hooks are dispatched from user_hooks.c. */
#pragma once

/* Applies the keymap's tap-hold defaults (config.h) on top of the Vial QMK Settings in RAM. */
void hrm_post_init(void);
