#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define LED_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

int main(void) {
    if (!gpio_is_ready_dt(&led)) return -1;

    if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) < 0) return -1;

    while (1) {
        gpio_pin_toggle_dt(&led);
        k_msleep(500);
    }

    return 0;
}
