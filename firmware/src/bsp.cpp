#include "bsp.hpp"

#include <zephyr/devicetree.h>

namespace bsp {

const gpio_dt_spec user_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
const device* const uart1 = DEVICE_DT_GET(DT_ALIAS(telemetry_uart));

} // namespace bsp
