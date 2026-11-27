# Liatris (RP2040): use CONVERT_TO so the full 11-layer Vial map and RGB framebuffer fit; keymap rules load too late for QMK to retarget.
#   qmk compile -kb splitkb/aurora/corne/rev1 -km vial_full -e CONVERT_TO=liatris
ifeq ($(strip $(CONVERT_TO)),liatris)
    SRC += liatris_led.c
endif

VIA_ENABLE = yes
VIAL_ENABLE = yes
VIALRGB_ENABLE = yes

ENCODER_MAP_ENABLE = yes

# Tap-hold keys held and released alone tap the key at the same position on the last layer.
SRC += hold_alone.c

# Home-row mods: Chordal Hold handedness (thumbs exempt), Flow Tap key filter, QMK Settings defaults.
SRC += hrm.c

# Passive mod-tap misfire counters (RAM only), shown on the right-half OLED.
SRC += misfire.c

# Firmware version over raw HID and on the OLEDs at power-up.
SRC += fw_version.c

# Keymap-level raw HID commands (host_hid.h) and the host-driven text on the right-half OLED.
SRC += host_hid.c host_status.c

# Right-half OLED: typing speed, layer lock / one-shot status, firmware version.
SRC += oled_status.c
WPM_ENABLE = yes
# QK_LLCK in the keymap is a no-op without this.
LAYER_LOCK_ENABLE = yes

# On-board layer/symbol drill; toggle with QK_KB_1 ("Trainer" in Vial).
SRC += trainer.c

# Game mode: QWERTY layers 6-9 with a chat detour; toggle with QK_KB_2 ("Game" in Vial). Display/RGB in game_display.c.
SRC += game_mode.c game_display.c

# Single owner of the shared QMK user hooks; dispatches to the modules above.
SRC += user_hooks.c
