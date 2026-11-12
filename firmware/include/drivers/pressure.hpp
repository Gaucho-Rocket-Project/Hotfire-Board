#ifndef DRIVERS_PRESSURE_HPP
#define DRIVERS_PRESSURE_HPP

#include <stddef.h>
#include <stdint.h>

class PressureTransducers {
public:
    static constexpr size_t CHANNEL_COUNT = 4;

    struct Reading {
        uint16_t raw = 0;
        int32_t current_ua = 0;
    };

    // Configure all inputs before starting the logger. Attempts every channel.
    int init();
    // Channel order: P0/J5, P1/J9, P2/J10, P3/J7 (PC0..PC3).
    // A successful read measures current; it does not certify sensor connection.
    int read(size_t channel, Reading& reading) const;

private:
    bool ready_[CHANNEL_COUNT]{};
};

#endif
