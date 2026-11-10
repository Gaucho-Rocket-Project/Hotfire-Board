
#include "drivers/uart.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>

#include <errno.h>

int UART::init() const
{
    if (!device_is_ready(uart_)) {
        return -ENODEV;
    }

    return 0;
}

void UART::write(
    const uint8_t* data,
    size_t length) const
{
    for (size_t i = 0; i < length; ++i) {
        uart_poll_out(uart_, data[i]);
    }
}

int UART::read_byte(uint8_t& byte) const
{
    unsigned char value = 0;

    int ret = uart_poll_in(uart_, &value);

    if (ret == 0) {
        byte = value;
    }

    return ret;
}
