#ifndef BSP_HPP
#define BSP_HPP

#include <stddef.h>
#include <stdint.h>

struct device;
struct gpio_dt_spec;
struct spi_config;
struct adc_dt_spec;

namespace bsp {

extern const gpio_dt_spec user_led;
extern const device* const uart1;
extern const device* const rtc;
extern const device* const entropy;
extern const gpio_dt_spec sd_card_detect;

extern const device* const sensor_spi;
extern const spi_config thermocouple_spi_config;
extern const spi_config accelerometer_spi_config;
extern const uint8_t thermocouple_decoder_addresses[4];
extern const uint8_t accelerometer_decoder_address;

// The decoder's low address bits share a port and must be updated together.
struct DecoderAddressPort {
    const device* port;
    uint8_t first_pin;
    uint8_t pin_count;
    uint32_t mask;
};
extern const DecoderAddressPort spi_decoder_low;
extern const gpio_dt_spec spi_decoder_a3;

inline constexpr size_t pressure_channel_count = 4;
extern const adc_dt_spec pressure_inputs[pressure_channel_count];
extern const char* const pressure_channel_labels[pressure_channel_count];

struct ExpansionGpio {
    const device* port;
    uint8_t pin;
    const char* id;
    const char* name;
    unsigned header_pin;
};
inline constexpr size_t expansion_gpio_count = 2;
extern const ExpansionGpio expansion_gpios[expansion_gpio_count];

extern const size_t ram_bytes;
extern const size_t flash_bytes;

} // namespace bsp

#endif // BSP_HPP
