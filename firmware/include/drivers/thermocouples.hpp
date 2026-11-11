#ifndef DRIVER_THERMOCOUPLES_H
#define DRIVER_THERMOCOUPLES_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>

class Thermocouples {
public:
    static constexpr size_t CHANNEL_COUNT = 4;

    struct Reading {
        int32_t temperature_mc = 0;
        uint8_t fault = 0;
        int error = 0;
        bool valid = false;
    };

    Thermocouples();

    // Configure all four MAX31856 amplifiers.
    int init();

    // Read a single amplifier: channel 0 through 3.
    int read(size_t channel, Reading& result);

    // Read all four amplifiers.
    // Attempts all channels even if one fails.
    int read_all(Reading (&results)[CHANNEL_COUNT]);

    int read_cold_junction(size_t channel, int32_t& temperature_mc);

private:
    const struct device* spi_;
    const struct device* gpioa_;
    const struct device* gpioc_;

    struct spi_config spi_config_{};
    struct k_mutex mutex_{};

    bool initialized_ = false;

    int select_channel(uint8_t channel);
    int deselect_all();

    int transfer(uint8_t channel, uint8_t* tx, uint8_t* rx, size_t length);

    int write_register(uint8_t channel, uint8_t address, uint8_t value);
    int read_registers(uint8_t channel, uint8_t address, uint8_t* data, size_t length);
}; // class Theromcouples

#endif // DRIVER_THERMOCOUPLES_H