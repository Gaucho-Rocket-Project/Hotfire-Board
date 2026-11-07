#include <zephyr/kernel.h>

#include "drivers/led.hpp"

static const gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int main()
{
    LED status_led(led);

    if (status_led.init() < 0) {
        return 0;
    }

    while (true) {
        status_led.toggle();
        k_msleep(500);
    }

    return 0;
}