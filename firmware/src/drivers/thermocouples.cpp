
#include "drivers/thermocouples.hpp"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(thermocouples, LOG_LEVEL_INF);

namespace {

constexpr uint8_t REG_CR0 = 0x00;
constexpr uint8_t REG_CR1 = 0x01;
constexpr uint8_t REG_MASK = 0x02;
constexpr uint8_t REG_TEMPERATURE = 0x0C;

// Type K, one conversion sample averaged.
constexpr uint8_t CR1_TYPE_K = 0x03;

// Automatic conversion + open-circuit detection.
// 60 Hz rejection, internal cold-junction compensation.
constexpr uint8_t CR0_CONFIG = 0x90;

constexpr uint32_t SPI_FREQUENCY = 1000000;

// PA5, PA6, PA7 = decoder A0, A1, A2.
constexpr gpio_port_pins_t ADDRESS_MASK = BIT(5) | BIT(6) | BIT(7);

// PC4 = decoder A3.
constexpr gpio_pin_t ADDRESS_A3 = 4;

} // namespace

Thermocouples::Thermocouples()
    : spi_(DEVICE_DT_GET(DT_NODELABEL(spi2))),
      gpioa_(DEVICE_DT_GET(DT_NODELABEL(gpioa))),
      gpioc_(DEVICE_DT_GET(DT_NODELABEL(gpioc)))
{
    k_mutex_init(&mutex_);

    // MAX31856 SPI mode 1:
    // CPOL = 0, CPHA = 1, MSB first.
    spi_config_.frequency = SPI_FREQUENCY;
    spi_config_.operation =
        SPI_OP_MODE_CONTROLLER |
        SPI_WORD_SET(8) |
        SPI_TRANSFER_MSB |
        SPI_MODE_CPHA;

    spi_config_.peripheral = 0;

    // No ordinary SPI CS:
    // The external CD74HC154 selects each device.
}

int Thermocouples::deselect_all()
{
    // PC4 first, then PA5-PA7.
    // Decoder address becomes 1111.
    int ret = gpio_pin_set(gpioc_, ADDRESS_A3, 1);

    if (ret < 0) {
        return ret;
    }

    return gpio_port_set_masked_raw(
        gpioa_,
        ADDRESS_MASK,
        ADDRESS_MASK
    );
}

int Thermocouples::select_channel(uint8_t channel)
{
    if (channel >= CHANNEL_COUNT) {
        return -EINVAL;
    }

    // Update PA5-PA7 together, while PC4 is still high.
    gpio_port_value_t address =
        static_cast<gpio_port_value_t>(
            (channel & 0x07U) << 5
        );

    int ret = gpio_port_set_masked_raw(
        gpioa_,
        ADDRESS_MASK,
        address
    );

    if (ret < 0) {
        return ret;
    }

    // Set A3 = 0 to select Y0-Y3.
    return gpio_pin_set(gpioc_, ADDRESS_A3, 0);
}

int Thermocouples::transfer(
    uint8_t channel,
    uint8_t* tx,
    uint8_t* rx,
    size_t length)
{
    k_mutex_lock(&mutex_, K_FOREVER);

    int ret = select_channel(channel);

    if (ret < 0) {
        (void)deselect_all();
        k_mutex_unlock(&mutex_);
        return ret;
    }

    struct spi_buf tx_buffer{};
    tx_buffer.buf = tx;
    tx_buffer.len = length;

    struct spi_buf rx_buffer{};
    rx_buffer.buf = rx;
    rx_buffer.len = length;

    struct spi_buf_set tx_set{};
    tx_set.buffers = &tx_buffer;
    tx_set.count = 1;

    struct spi_buf_set rx_set{};
    rx_set.buffers = &rx_buffer;
    rx_set.count = 1;

    // Hold the decoder CS active for the entire transfer.
    ret = spi_transceive(
        spi_,
        &spi_config_,
        &tx_set,
        &rx_set
    );

    // Always end the SPI transaction by deselecting.
    int cs_ret = deselect_all();

    k_mutex_unlock(&mutex_);

    return (ret < 0) ? ret : cs_ret;
}

int Thermocouples::write_register(
    uint8_t channel,
    uint8_t address,
    uint8_t value)
{
    uint8_t tx[2] = {
        static_cast<uint8_t>(address | 0x80U),
        value
    };

    uint8_t rx[2]{};

    return transfer(channel, tx, rx, sizeof(tx));
}

int Thermocouples::read_registers(
    uint8_t channel,
    uint8_t address,
    uint8_t* data,
    size_t length)
{
    if (data == nullptr || length == 0 || length > 4) {
        return -EINVAL;
    }

    uint8_t tx[5]{};
    uint8_t rx[5]{};

    // MSB clear indicates a register read.
    tx[0] = address & 0x7FU;

    int ret = transfer(
        channel,
        tx,
        rx,
        length + 1
    );

    if (ret < 0) {
        return ret;
    }

    // RX byte 0 corresponds to the address transfer.
    memcpy(data, &rx[1], length);

    return 0;
}

int Thermocouples::init()
{
    if (!device_is_ready(spi_) ||
        !device_is_ready(gpioa_) ||
        !device_is_ready(gpioc_)) {

        LOG_ERR("SPI2 or decoder GPIO unavailable");
        return -ENODEV;
    }

    // Initialize A3 first. External pull-ups keep all
    // address lines high at boot, selecting idle Y15.
    int ret = gpio_pin_configure(
        gpioc_,
        ADDRESS_A3,
        GPIO_OUTPUT_HIGH
    );

    if (ret < 0) {
        return ret;
    }

    for (uint8_t pin = 5; pin <= 7; ++pin) {
        ret = gpio_pin_configure(
            gpioa_,
            pin,
            GPIO_OUTPUT_HIGH
        );

        if (ret < 0) {
            return ret;
        }
    }

    ret = deselect_all();

    if (ret < 0) {
        return ret;
    }

    // Configure each amplifier independently.
    for (uint8_t ch = 0; ch < CHANNEL_COUNT; ++ch) {

        // Thermocouple type K, averaging = 1.
        ret = write_register(ch, REG_CR1, CR1_TYPE_K);

        if (ret < 0) {
            return ret;
        }

        // Unmask faults for the FAULT output.
        // We read the fault status register over SPI.
        ret = write_register(ch, REG_MASK, 0x00);

        if (ret < 0) {
            return ret;
        }

        // Start continuous temperature conversions.
        ret = write_register(ch, REG_CR0, CR0_CONFIG);

        if (ret < 0) {
            return ret;
        }

        // Verify that this device accepted its config.
        uint8_t cr0 = 0;
        uint8_t cr1 = 0;

        ret = read_registers(ch, REG_CR0, &cr0, 1);

        if (ret < 0) {
            return ret;
        }

        ret = read_registers(ch, REG_CR1, &cr1, 1);

        if (ret < 0) {
            return ret;
        }

        if (cr0 != CR0_CONFIG || cr1 != CR1_TYPE_K) {
            LOG_ERR(
                "TC%u configuration readback failed",
                static_cast<unsigned>(ch + 1)
            );

            return -EIO;
        }

        LOG_INF(
            "TC%u MAX31856 configured",
            static_cast<unsigned>(ch + 1)
        );
    }

    // Allow initial conversions to complete.
    k_msleep(200);

    initialized_ = true;

    LOG_INF("All four MAX31856 channels initialized");

    return 0;
}

int Thermocouples::read(size_t channel, Reading& result)
{
    result = {};

    if (!initialized_) {
        result.error = -ENODEV;
        return result.error;
    }

    if (channel >= CHANNEL_COUNT) {
        result.error = -EINVAL;
        return result.error;
    }

    // Read 0x0C through 0x0F in one SPI transaction:
    // three temperature bytes + fault status.
    uint8_t data[4]{};

    int ret = read_registers(
        static_cast<uint8_t>(channel),
        REG_TEMPERATURE,
        data,
        sizeof(data)
    );

    if (ret < 0) {
        result.error = ret;
        return ret;
    }

    result.fault = data[3];

    // Assemble the signed 19-bit temperature result.
    uint32_t packed =
        (static_cast<uint32_t>(data[0]) << 16) |
        (static_cast<uint32_t>(data[1]) << 8) |
        static_cast<uint32_t>(data[2]);

    int32_t raw = static_cast<int32_t>(packed >> 5);

    // Sign extension from 19 bits.
    if ((raw & (1 << 18)) != 0) {
        raw -= (1 << 19);
    }

    // Each LSB = 1/128 degrees C.
    // Store temperature in milli-degrees Celsius.
    result.temperature_mc = (raw * 1000) / 128;

    if (result.fault != 0) {
        result.error = -EIO;
        result.valid = false;
        return result.error;
    }

    result.error = 0;
    result.valid = true;

    return 0;
}

int Thermocouples::read_all(Reading (&results)[CHANNEL_COUNT])
{
    int first_error = 0;

    for (size_t ch = 0; ch < CHANNEL_COUNT; ++ch) {
        int ret = read(ch, results[ch]);

        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }

    return first_error;
}


int Thermocouples::read_cold_junction(size_t channel, int32_t& temperature_mc)
{
    temperature_mc = 0;

    if (!initialized_) {
        return -ENODEV;
    }

    if (channel >= CHANNEL_COUNT) {
        return -EINVAL;
    }

    // MAX31856:
    // 0x0A = Cold-junction temperature MSB
    // 0x0B = Cold-junction temperature LSB
    constexpr uint8_t REG_CJ_TEMP = 0x0A;

    uint8_t data[2]{};

    int ret = read_registers(
        static_cast<uint8_t>(channel),
        REG_CJ_TEMP,
        data,
        sizeof(data)
    );

    if (ret < 0) {
        return ret;
    }

    // The 14-bit signed result occupies:
    // MSB[7:0], LSB[7:2].
    int32_t raw =
        (static_cast<int32_t>(data[0]) << 6) |
        (static_cast<int32_t>(data[1]) >> 2);

    // Sign-extend the 14-bit two's-complement value.
    if (raw & (1 << 13)) {
        raw -= (1 << 14);
    }

    // Resolution = 1/64 C per count.
    // Convert to milli-degrees Celsius.
    temperature_mc = (raw * 1000) / 64;

    return 0;
}
