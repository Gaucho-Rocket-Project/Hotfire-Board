#ifndef DRIVER_THERMOCOUPLES_H
#define DRIVER_THERMOCOUPLES_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>

// MAX31856 CR1 thermocouple-type encodings; averaging remains one sample.
enum class ThermocoupleType : uint8_t {
    B = 0, E = 1, J = 2, K = 3, N = 4, R = 5, S = 6, T = 7
};

inline const char* thermocouple_type_name(ThermocoupleType type)
{
    switch (type) {
    case ThermocoupleType::B: return "B";
    case ThermocoupleType::E: return "E";
    case ThermocoupleType::J: return "J";
    case ThermocoupleType::K: return "K";
    case ThermocoupleType::N: return "N";
    case ThermocoupleType::R: return "R";
    case ThermocoupleType::S: return "S";
    case ThermocoupleType::T: return "T";
    }
    return "?";
}

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
    // Configuration is immutable after the boot-time init attempt.
    bool channel_enabled(size_t channel) const;

    struct Reading {
        int32_t temperature_mc = 0;
        uint8_t fault = 0;
        int error = 0;
        bool valid = false;
        bool fault_valid = false;
    };

    struct StartupStatus {
        int error = -EAGAIN;
        const char* stage = "not attempted";
        const char* bus_stage = nullptr;
        uint8_t channel = CHANNEL_COUNT; // CHANNEL_COUNT means no channel selected.
        uint8_t expected_cr0 = 0;
        uint8_t expected_cr1 = 0;
        uint8_t cr0 = 0;
        uint8_t cr1 = 0;
        bool cr0_valid = false;
        bool cr1_valid = false;
    };

    // Immutable after boot initialization, retained even if shell logs are dropped.
    const StartupStatus& startup_status() const { return startup_; }

    Thermocouples();

    // Call once at boot, before binding the shell or starting acquisition.
    // Disabled amplifiers are not accessed; this does not power them down.
    int init(const ThermocoupleConfig& config);
    uint8_t enabled_mask() const;

    // Read a single amplifier: channel 0 through 3; disabled channels return -ENODEV.
    int read(size_t channel, Reading& result);

    // Read enabled amplifiers; disabled entries report -ENODEV without failing the group.
    int read_all(Reading (&results)[CHANNEL_COUNT]);

    int read_cold_junction(size_t channel, int32_t& temperature_mc);

private:
    StartupStatus startup_{};
    ThermocoupleConfig config_{};
    uint8_t enabled_mask_ = 0;
    bool init_attempted_ = false;
    bool initialized_ = false;

    int configure(const ThermocoupleConfig& config);

    int transfer(uint8_t channel, uint8_t* tx, uint8_t* rx, size_t length);

    int write_register(uint8_t channel, uint8_t address, uint8_t value);
    int read_registers(uint8_t channel, uint8_t address, uint8_t* data, size_t length);
}; // class Theromcouples

#endif // DRIVER_THERMOCOUPLES_H
