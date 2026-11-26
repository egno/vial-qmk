/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Right-half OLED status screen; hooks are dispatched from user_hooks.c. */
#pragma once

#include <stdbool.h>

void status_post_init(void);
void status_housekeeping(void);
/* Returns false when it drew the screen (secondary half), true to let corne.c render. */
bool status_oled_task(void);
