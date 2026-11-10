#ifndef BSP_HPP
#define BSP_HPP

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

namespace bsp {

extern const gpio_dt_spec user_led;
extern const device* const uart1;

} // namespace bsp

#endif // BSP_HPP
