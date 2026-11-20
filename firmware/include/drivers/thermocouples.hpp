#ifndef DRIVER_THERMOCOUPLES_H
#define DRIVER_THERMOCOUPLES_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>

enum class ThermocoupleType { B, E, J, K, N, R, S, T };

struct ThermocoupleChannelConfig {
    bool enabled = true;
    ThermocoupleType type = ThermocoupleType::K;
};

struct ThermocoupleConfig {
    ThermocoupleChannelConfig channels[4];
};

class Thermocouples {
public:
    static constexpr size_t CHANNEL_COUNT = 4;
    // Bit 0=TC1 through bit 3=TC4. 0x07 tests TC1-TC3; 0x0f enables all four.
    static constexpr uint8_t ENABLED_CHANNEL_MASK = 0x0f;

    static constexpr bool channel_enabled(size_t channel)
    {
        return channel < CHANNEL_COUNT && (ENABLED_CHANNEL_MASK & (1U << channel));
    }

    struct Reading {
        int32_t temperature_mc = 0;
        uint8_t fault = 0;
        int error = 0;
        bool valid = false;
    };

    Thermocouples();

    // Configure only enabled MAX31856 amplifiers.
    int init(const ThermocoupleConfig& config);
    uint8_t enabled_mask() const;

    // Read a single amplifier: channel 0 through 3; disabled channels return -ENODEV.
    int read(size_t channel, Reading& result);

    // Read enabled amplifiers; disabled entries report -ENODEV without failing the group.
    int read_all(Reading (&results)[CHANNEL_COUNT]);

    int read_cold_junction(size_t channel, int32_t& temperature_mc);

private:
    struct k_mutex mutex_{};

    bool initialized_ = false;

    int select_channel(uint8_t channel);
    int deselect_all();

    int transfer(uint8_t channel, uint8_t* tx, uint8_t* rx, size_t length);

    int write_register(uint8_t channel, uint8_t address, uint8_t value);
    int read_registers(uint8_t channel, uint8_t address, uint8_t* data, size_t length);
}; // class Theromcouples

#endif // DRIVER_THERMOCOUPLES_H
