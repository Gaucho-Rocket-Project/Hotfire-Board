#ifndef DRIVERS_UART_H
#define DRIVERS_UART_H

#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include <stddef.h>
#include <stdint.h>

class UART {
public:
    explicit UART(const device* const uart) noexcept : uart_(uart) {}

    int init();

    void write(const uint8_t* data, size_t length) const;

    // Returns 0 if a byte was received.
    // Returns -EAGAIN if empty, -EOVERFLOW if data was lost (resync required).
    int read_byte(uint8_t& byte);

private:
    const struct device* const uart_;
    ring_buf rx_{};
    uint8_t rx_storage_[256]{};
    atomic_t rx_error_{};

    static void rx_interrupt(const device* dev, void* context);
};

#endif // DRIVERS_UART_H
