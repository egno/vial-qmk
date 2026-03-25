# Liatris (RP2040): use CONVERT_TO so the full 7-layer Vial map and RGB framebuffer fit; keymap rules load too late for QMK to retarget.
#   qmk compile -kb splitkb/aurora/corne/rev1 -km vial_full -e CONVERT_TO=liatris
ifeq ($(strip $(CONVERT_TO)),liatris)
    SRC += liatris_led.c
endif

VIA_ENABLE = yes
VIAL_ENABLE = yes
VIALRGB_ENABLE = yes

ENCODER_MAP_ENABLE = yes
