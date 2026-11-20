#pragma once
#include <cstddef>
#include <cstdint>

struct device {};
inline device fake_spi, fake_gpioa, fake_gpioc;
#define DT_NODELABEL(name) name
#define FAKE_DEVICE_GET(node) (&fake_##node)
#define DEVICE_DT_GET(node) FAKE_DEVICE_GET(node)
#define fake_spi2 fake_spi
inline bool device_is_ready(const device*) { return true; }
struct k_mutex {};
constexpr int K_FOREVER = -1;
inline void k_mutex_init(k_mutex*) {}
inline int k_mutex_lock(k_mutex*, int) { return 0; }
inline int k_mutex_unlock(k_mutex*) { return 0; }
inline void k_msleep(int) {}

using gpio_port_pins_t = uint32_t;
using gpio_port_value_t = uint32_t;
using gpio_pin_t = uint8_t;
constexpr int GPIO_OUTPUT_HIGH = 1;
constexpr int GPIO_OUTPUT_ACTIVE = GPIO_OUTPUT_HIGH;
struct gpio_dt_spec { const device* port; gpio_pin_t pin; uint32_t dt_flags; };
int gpio_pin_configure(const device*, gpio_pin_t, int);
int gpio_pin_set(const device*, gpio_pin_t, int);
int gpio_port_set_masked_raw(const device*, gpio_port_pins_t, gpio_port_value_t);
inline bool gpio_is_ready_dt(const gpio_dt_spec* spec) { return device_is_ready(spec->port); }
inline int gpio_pin_configure_dt(const gpio_dt_spec* spec, int flags) {
    return gpio_pin_configure(spec->port, spec->pin, flags);
}
inline int gpio_pin_set_dt(const gpio_dt_spec* spec, int value) {
    return gpio_pin_set(spec->port, spec->pin, value);
}

struct spi_config { uint32_t frequency; uint32_t operation; uint16_t peripheral; };
struct spi_buf { void* buf; size_t len; };
struct spi_buf_set { const spi_buf* buffers; size_t count; };
constexpr uint32_t SPI_OP_MODE_CONTROLLER = 0, SPI_TRANSFER_MSB = 0, SPI_MODE_CPHA = 1;
#define SPI_WORD_SET(bits) ((bits) << 5)
int spi_transceive(const device*, const spi_config*, const spi_buf_set*, const spi_buf_set*);

#define BIT(n) (1U << (n))
#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...)
#define LOG_ERR(...)
#define LOG_WRN(...)
