#ifndef DRIVERS_UART_H
#define DRIVERS_UART_H

#include <zephyr/device.h>

#include <stddef.h>
#include <stdint.h>

class UART {
public:
    explicit constexpr UART(const device* const uart) noexcept : uart_(uart) {}

    int init() const;

    void write(const uint8_t* data, size_t length) const;

    // Returns 0 if a byte was received.
    // Returns -1 if no data is available.
    int read_byte(uint8_t& byte) const;

    private:
    const struct device* const uart_;
};

#endif // DRIVERS_UART_H
