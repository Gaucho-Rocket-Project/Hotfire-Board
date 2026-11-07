#include "drivers/led.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include <errno.h>


int LED::init() 
{
    if (!gpio_is_ready_dt(&led_)) return -ENODEV;
    return gpio_pin_configure_dt(&led_, GPIO_OUTPUT_INACTIVE);
}

int LED::on() 
{
    return gpio_pin_set_dt(&led_, 1);
}

int LED::off() 
{
    return gpio_pin_set_dt(&led_, 0);
}

int LED::toggle() 
{
    return gpio_pin_toggle_dt(&led_);
}
