/* Liatris power LED on GPIO 24 (active low). See:
 * https://docs.splitkb.com/product-guides/liatris/power-led
 */
#include "quantum.h"

#ifdef CONVERT_TO_LIATRIS
#    include "gpio.h"

void keyboard_pre_init_user(void) {
    gpio_set_pin_output(24);
    gpio_write_pin_high(24);
}
#endif
