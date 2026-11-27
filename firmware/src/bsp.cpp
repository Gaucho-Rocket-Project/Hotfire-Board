#include "bsp.hpp"

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>

namespace bsp {

const gpio_dt_spec user_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
const device* const uart1 = DEVICE_DT_GET(DT_ALIAS(telemetry_uart));
const device* const rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
const device* const entropy = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
const gpio_dt_spec sd_card_detect = GPIO_DT_SPEC_GET(DT_NODELABEL(sdmmc1), cd_gpios);

const device* const sensor_spi = DEVICE_DT_GET(DT_NODELABEL(spi2));
// MAX31856: 1 MHz, mode 3, 8-bit MSB first. The CD74HC154 controls CS.
// Both sensors support mode 3; keep SCK polarity consistent on the shared bus.
const spi_config thermocouple_spi_config = [] {
    spi_config config{};
    config.frequency = 1000000;
    config.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |
                       SPI_TRANSFER_MSB | SPI_MODE_CPOL | SPI_MODE_CPHA;
    config.peripheral = 0;
    return config;
}();

// ADXL345: same bus, 1 MHz requested, mode 3. Decoder output Y13 is board net CS13.
const spi_config accelerometer_spi_config = [] {
    spi_config config{};
    config.frequency = 1000000;
    config.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |
                       SPI_TRANSFER_MSB | SPI_MODE_CPOL | SPI_MODE_CPHA;
    config.peripheral = 0;
    return config;
}();
const uint8_t thermocouple_decoder_addresses[4] = {0, 1, 2, 3};
const uint8_t accelerometer_decoder_address = 13;

// A0-A2: PA5-PA7. A3: PC4. Y4-Y7 and Y15 are reserved for parking; address 15 is idle.
const DecoderAddressPort spi_decoder_low = {
    DEVICE_DT_GET(DT_NODELABEL(gpioa)), 5, 3, BIT(5) | BIT(6) | BIT(7)
};
const gpio_dt_spec spi_decoder_a3 = {
    DEVICE_DT_GET(DT_NODELABEL(gpioc)), 4, GPIO_ACTIVE_HIGH
};

constexpr adc_dt_spec pressure_inputs[pressure_channel_count] = {
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0),
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1),
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 2),
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 3),
};
static_assert(pressure_inputs[0].channel_id == 1 && pressure_inputs[1].channel_id == 2 &&
              pressure_inputs[2].channel_id == 3 && pressure_inputs[3].channel_id == 4,
              "Pressure scan packing assumes ADC channels 1..4 in ascending order");
const char* const pressure_channel_labels[pressure_channel_count] = {
    "J5, PC0 / ADC1 IN1", "J9, PC1 / ADC1 IN2",
    "J10, PC2 / ADC1 IN3", "J7, PC3 / ADC1 IN4"
};

const ExpansionGpio expansion_gpios[expansion_gpio_count] = {
    {DEVICE_DT_GET(DT_NODELABEL(gpiob)), 0, "0", "PB0", 6},
    {DEVICE_DT_GET(DT_NODELABEL(gpioc)), 5, "1", "PC5", 7},
};

const size_t ram_bytes = DT_REG_SIZE(DT_CHOSEN(zephyr_sram));
const size_t flash_bytes = DT_REG_SIZE(DT_CHOSEN(zephyr_flash));

} // namespace bsp
