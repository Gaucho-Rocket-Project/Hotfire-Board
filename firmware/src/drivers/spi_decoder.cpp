#include "drivers/spi_decoder.hpp"
#include "bsp.hpp"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <errno.h>

namespace {
K_MUTEX_DEFINE(bus_mutex);
bool initialized = false;
const spi_config* active_config = nullptr;

int deselect()
{
    // Board population: Y0-Y3 and Y13. Park through unused outputs, changing
    // only one address bit at a time: A2, A1, A0, then A3 high. This avoids
    // selecting another device while moving between the two address banks.
    const auto& low = bsp::spi_decoder_low;
    for (int bit = 2; bit >= 0; --bit) {
        const int ret = gpio_pin_set(low.port, low.first_pin + bit, 1);
        if (ret < 0) {
            return ret;
        }
    }
    return gpio_pin_set_dt(&bsp::spi_decoder_a3, 1);
}

int select(uint8_t address)
{
    // Reverse the parking path from Y15: A3, A0, A1, then A2.
    // TC selection happens on the final A2 edge; Y13 uses A3=1, A2:A0=101.
    int ret = gpio_pin_set_dt(&bsp::spi_decoder_a3, (address >> 3) & 1U);
    const auto& low = bsp::spi_decoder_low;
    for (uint8_t bit = 0; ret == 0 && bit < 3; ++bit) {
        ret = gpio_pin_set(low.port, low.first_pin + bit, (address >> bit) & 1U);
    }
    return ret;
}

bool supported_address(uint8_t address)
{
    // The parking paths are designed for the five populated board outputs.
    // Do not permit arbitrary decoder addresses to pass through a live CS.
    if (address == bsp::accelerometer_decoder_address) {
        return address == 13;
    }
    for (const auto tc_address : bsp::thermocouple_decoder_addresses) {
        if (address == tc_address) {
            return address < 4;
        }
    }
    return false;
}

int exchange(const spi_config& config, uint8_t* tx, uint8_t* rx, size_t length)
{
    spi_buf tx_buffer{tx, length};
    spi_buf rx_buffer{rx, length};
    const spi_buf_set tx_set{&tx_buffer, 1};
    const spi_buf_set rx_set{&rx_buffer, 1};
    return spi_transceive(bsp::sensor_spi, &config, &tx_set, &rx_set);
}
}

int spi_decoder::initialize(const char** failure_stage)
{
    k_mutex_lock(&bus_mutex, K_FOREVER);
    const char* stage = "SPI/GPIO device readiness";
    const int ret = [&]() -> int {
        if (initialized) {
            return 0;
        }
        const auto& low = bsp::spi_decoder_low;
        if (!device_is_ready(bsp::sensor_spi) || !device_is_ready(low.port) ||
            !gpio_is_ready_dt(&bsp::spi_decoder_a3)) {
            return -ENODEV;
        }
        stage = "configure decoder A3";
        int result = gpio_pin_configure_dt(&bsp::spi_decoder_a3, GPIO_OUTPUT_ACTIVE);
        for (uint8_t i = 0; result == 0 && i < low.pin_count; ++i) {
            stage = "configure decoder A0-A2";
            result = gpio_pin_configure(low.port, low.first_pin + i, GPIO_OUTPUT_HIGH);
        }
        if (result == 0) {
            stage = "park decoder at Y15";
            result = deselect();
        }
        initialized = result == 0;
        return result;
    }();
    if (failure_stage) { *failure_stage = ret < 0 ? stage : nullptr; }
    k_mutex_unlock(&bus_mutex);
    return ret;
}

int spi_decoder::transfer(uint8_t address, const spi_config& config,
                          uint8_t* tx, uint8_t* rx, size_t length, const char** failure_stage)
{
    if (failure_stage) { *failure_stage = "validate SPI transfer"; }
    if (!supported_address(address) || tx == nullptr || rx == nullptr || length == 0) {
        return -EINVAL;
    }
    k_mutex_lock(&bus_mutex, K_FOREVER);
    const char* stage = initialized ? "park decoder before transfer" : "SPI decoder not initialized";
    int ret = initialized ? deselect() : -ENODEV;
    if (ret == 0 && active_config != &config) {
        // Zephyr configures SCK polarity inside spi_transceive(). Prime the
        // controller with a dummy byte at idle Y15 before asserting real CS,
        // so a mode change cannot create an extra clock edge at a sensor.
        uint8_t dummy_tx = 0, dummy_rx = 0;
        stage = "SPI mode setup at idle Y15";
        ret = exchange(config, &dummy_tx, &dummy_rx, 1);
        if (ret == 0) {
            active_config = &config;
        }
    }
    if (ret == 0) {
        stage = "select decoder output";
        ret = select(address);
    }
    if (ret == 0) {
        stage = "SPI data transfer";
        ret = exchange(config, tx, rx, length);
    }
    if (initialized) {
        const int idle_ret = deselect();
        if (ret == 0) {
            stage = "deselect decoder output";
            ret = idle_ret;
        }
    }
    if (failure_stage) { *failure_stage = ret < 0 ? stage : nullptr; }
    if (ret < 0) {
        active_config = nullptr;
    }
    k_mutex_unlock(&bus_mutex);
    return ret;
}
