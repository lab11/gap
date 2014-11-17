// LED Library

// Assumes active low LEDs

#include <stdint.h>
#include "nrf_gpio.h"


void led_init (uint32_t pin_number) {
	nrf_gpio_cfg_output(pin_number);
    nrf_gpio_pin_set(pin_number);
}

void led_on (uint32_t pin_number) {
    nrf_gpio_pin_clear(pin_number);
}

void led_off (uint32_t pin_number) {
    nrf_gpio_pin_set(pin_number);
}

void led_toggle (uint32_t pin_number) {
    nrf_gpio_pin_toggle(pin_number);
}
