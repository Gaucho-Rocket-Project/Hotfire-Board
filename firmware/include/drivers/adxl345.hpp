#ifndef DRIVERS_ADXL345_HPP
#define DRIVERS_ADXL345_HPP

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/kernel.h>

class ADXL345 {
public:
    static constexpr unsigned RATE_HZ = 800;
    static constexpr unsigned FIFO_DEPTH = 32;
    struct Reading {
        int16_t raw[3]{};
        int32_t mg[3]{}; // Nominal 4 mg/raw count in full-resolution mode.
        bool fifo_overrun = false; // FIFO reached full; exact lost count unknown.
    };
    struct StartupStatus {
        int error = -EAGAIN;
        const char* stage = "not attempted";
        const char* bus_stage = nullptr;
        uint8_t reg = 0;
        uint8_t expected = 0;
        uint8_t actual = 0;
        bool readback_valid = false;
    };

    ADXL345();
    int init(); // Boot only: verify ID, configure 800 Hz, full-resolution +/-16 g.
    bool ready() const { return initialized_; }
    const StartupStatus& startup_status() const { return startup_; }
    int clear_fifo(); // Use before starting a new recording.
    int read(Reading& reading); // Oldest FIFO entry; -EAGAIN if empty.

private:
    k_mutex mutex_{};
    bool init_attempted_ = false;
    bool initialized_ = false;
    StartupStatus startup_{};
    int configure();
    int write_register(uint8_t address, uint8_t value);
    int read_registers(uint8_t address, uint8_t* data, size_t length);
};
#endif
