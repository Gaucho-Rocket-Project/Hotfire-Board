#ifndef DRIVERS_ADXL345_HPP
#define DRIVERS_ADXL345_HPP

#include <stdint.h>
#include <stddef.h>
#include <zephyr/kernel.h>

class ADXL345 {
public:
    static constexpr unsigned RATE_HZ = 1600;
    static constexpr unsigned FIFO_DEPTH = 32;
    struct Reading {
        int16_t raw[3]{};
        int32_t mg[3]{}; // Nominal 4 mg/raw count; at 1600 Hz bit 0 is zero (8 mg steps).
        bool fifo_overrun = false; // FIFO reached full; exact lost count unknown.
    };

    ADXL345();
    int init(); // Boot only: verify ID, configure 1600 Hz, full-resolution +/-16 g.
    bool ready() const { return initialized_; }
    int clear_fifo(); // Use before starting a new recording.
    int read(Reading& reading); // Oldest FIFO entry; -EAGAIN if empty.

private:
    k_mutex mutex_{};
    bool init_attempted_ = false;
    bool initialized_ = false;
    int write_register(uint8_t address, uint8_t value);
    int read_registers(uint8_t address, uint8_t* data, size_t length);
};
#endif
