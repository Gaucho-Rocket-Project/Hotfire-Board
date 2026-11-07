#ifndef DRIVERS_LED_H
#define DRIVERS_LED_H

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

class LED 
{
public:
    explicit constexpr LED(gpio_dt_spec led) noexcept : led_(led) {}
    
    int init();
    int on() noexcept;
    int off() noexcept;
    int toggle() noexcept;

private:
    gpio_dt_spec led_;
}; // class LED

#endif // DRIVERS_LED_H