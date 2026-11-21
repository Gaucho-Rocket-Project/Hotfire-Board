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
    // Only Y0-Y7 are used by this interface. Raising A3 disconnects them
    // before changing A0-A2; address 15 is reserved as the idle selection.
    int ret = gpio_pin_set_dt(&bsp::spi_decoder_a3, 1);
    if (ret < 0) {
        return ret;
    }
    const auto& low = bsp::spi_decoder_low;
    return gpio_port_set_masked_raw(low.port, low.mask, low.mask);
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

int spi_decoder::initialize()
{
    k_mutex_lock(&bus_mutex, K_FOREVER);
    const int ret = []() -> int {
        if (initialized) {
            return 0;
        }
        const auto& low = bsp::spi_decoder_low;
        if (!device_is_ready(bsp::sensor_spi) || !device_is_ready(low.port) ||
            !gpio_is_ready_dt(&bsp::spi_decoder_a3)) {
            return -ENODEV;
        }
        int result = gpio_pin_configure_dt(&bsp::spi_decoder_a3, GPIO_OUTPUT_ACTIVE);
        for (uint8_t i = 0; result == 0 && i < low.pin_count; ++i) {
            result = gpio_pin_configure(low.port, low.first_pin + i, GPIO_OUTPUT_HIGH);
        }
        if (result == 0) {
            result = deselect();
        }
        initialized = result == 0;
        return result;
    }();
    k_mutex_unlock(&bus_mutex);
    return ret;
}

int spi_decoder::transfer(uint8_t address, const spi_config& config,
                          uint8_t* tx, uint8_t* rx, size_t length)
{
    if (address >= 8 || tx == nullptr || rx == nullptr || length == 0) {
        return -EINVAL;
    }
    k_mutex_lock(&bus_mutex, K_FOREVER);
    int ret = initialized ? deselect() : -ENODEV;
    if (ret == 0 && active_config != &config) {
        // Zephyr configures SCK polarity inside spi_transceive(). Prime the
        // controller with a dummy byte at idle Y15 before asserting real CS,
        // so a mode change cannot create an extra clock edge at a sensor.
        uint8_t dummy_tx = 0, dummy_rx = 0;
        ret = exchange(config, &dummy_tx, &dummy_rx, 1);
        if (ret == 0) {
            active_config = &config;
        }
    }
    if (ret == 0) {
        const auto& low = bsp::spi_decoder_low;
        ret = gpio_port_set_masked_raw(low.port, low.mask,
                                      static_cast<uint32_t>(address) << low.first_pin);
    }
    if (ret == 0) {
        ret = gpio_pin_set_dt(&bsp::spi_decoder_a3, 0);
    }
    if (ret == 0) {
        ret = exchange(config, tx, rx, length);
    }
    if (initialized) {
        const int idle_ret = deselect();
        if (ret == 0) {
            ret = idle_ret;
        }
    }
    if (ret < 0) {
        active_config = nullptr;
    }
    k_mutex_unlock(&bus_mutex);
    return ret;
}
