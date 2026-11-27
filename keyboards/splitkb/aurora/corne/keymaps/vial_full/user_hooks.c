/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Single owner of the QMK user hooks shared by hold_alone.c, fw_version.c,
 * game_mode.c, host_hid.c, oled_status.c and trainer.c. trainer.c additionally owns oled_init_user and
 * rgb_matrix_indicators_advanced_user (chaining to game_display.c), host_hid.c raw_hid_receive_kb,
 * and game_display.c the keyboard-level mouse_jiggler_inhibited_user / corne_chat_mode_user,
 * which nothing else defines.
 */
#include "quantum.h"
#include "usb_device_state.h"
#include "fw_version.h"
#include "game_mode.h"
#include "hold_alone.h"
#include "host_hid.h"
#include "host_status.h"
#include "hrm.h"
#include "misfire.h"
#include "oled_status.h"
#include "trainer.h"

void keyboard_post_init_user(void) {
    hrm_post_init();
    misfire_post_init();
    status_post_init();
    host_status_post_init();
    trainer_post_init();
}

void housekeeping_task_user(void) {
    status_housekeeping();
    host_status_housekeeping();
    trainer_housekeeping();
    game_mode_housekeeping();
    /* Last, so the 0xF7 events carry what the modules above did in this pass. */
    host_hid_housekeeping();
}

void notify_usb_device_state_change_user(struct usb_device_state usb_device_state) {
    host_hid_usb_state(usb_device_state.configure_state == USB_DEVICE_STATE_CONFIGURED);
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    /* Hold-alone sees every event (including the trainer toggle) so a press
     * always cancels a pending action; it never consumes anything itself. */
    hold_alone_process_record(keycode, record);
    /* Observer only; ignores everything while the trainer is active. */
    misfire_process_record(keycode, record);
    /* After hold-alone, so the keys game mode consumes still cancel a pending action;
     * before the trainer, so the game keys stay inert (but consumed) while drilling. */
    if (!game_mode_process_record(keycode, record)) return false;
    return trainer_process_record(keycode, record);
}

void post_process_record_user(uint16_t keycode, keyrecord_t *record) {
    /* Hold-alone taps keys; nothing may reach the host while drilling. */
    if (trainer_is_active()) return;
    hold_alone_post_process_record(keycode, record);
    game_mode_post_process_record(keycode, record);
}

layer_state_t layer_state_set_user(layer_state_t state) {
    return game_mode_layer_state_set(state);
}

#ifdef OLED_ENABLE
bool oled_task_user(void) {
    /* Priority: version splash (both halves, power-up only), then the trainer
     * (master, while active), then the status screen (secondary). Whatever is
     * left - the master when idle - falls through to corne.c. */
    if (!fw_version_oled_task()) return false;
    if (!trainer_oled_task()) return false;
    return status_oled_task();
}
#endif
