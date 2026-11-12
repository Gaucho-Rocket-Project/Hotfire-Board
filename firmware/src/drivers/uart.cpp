
#include "drivers/uart.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>

#include <errno.h>

int UART::init()
{
    if (!device_is_ready(uart_)) {
        return -ENODEV;
    }

    ring_buf_init(&rx_, sizeof(rx_storage_), rx_storage_);
    atomic_clear(&rx_error_);
    int ret = uart_irq_callback_user_data_set(uart_, rx_interrupt, this);
    if (ret < 0) {
        return ret;
    }
    uart_irq_rx_enable(uart_);
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

void UART::rx_interrupt(const device* dev, void* context)
{
    auto* self = static_cast<UART*>(context);
    uart_irq_update(dev);
    if (uart_err_check(dev) > 0) {
        atomic_set(&self->rx_error_, 1);
    }
    while (uart_irq_rx_ready(dev)) {
        uint8_t bytes[16];
        int count = uart_fifo_read(dev, bytes, sizeof(bytes));
        if (count <= 0) {
            break;
        }
        if (ring_buf_put(&self->rx_, bytes, count) != static_cast<uint32_t>(count)) {
            atomic_set(&self->rx_error_, 1);
        }
    }
}

int UART::read_byte(uint8_t& byte)
{
    if (atomic_set(&rx_error_, 0)) {
        // Consumer-side drain preserves the single-producer/single-consumer
        // ring-buffer contract even if the ISR is receiving more bytes.
        uint8_t discarded[32];
        for (unsigned i = 0; i < sizeof(rx_storage_) / sizeof(discarded); ++i) {
            if (ring_buf_get(&rx_, discarded, sizeof(discarded)) == 0) {
                break;
            }
        }
        return -EOVERFLOW;
    }
    return ring_buf_get(&rx_, &byte, 1) == 1 ? 0 : -EAGAIN;
}
