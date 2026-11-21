#include "bsp.hpp"
#include "drivers/thermocouples.hpp"
#include "drivers/spi_decoder.hpp"

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
constexpr uint8_t REG_FAULT_STATUS = 0x0F;
constexpr uint8_t FAULT_OVUV = BIT(1);

constexpr uint8_t CR0_AUTOMATIC = BIT(7);
// Table 4: OCFAULT=01 for input series resistance below 5 kohm.
// Board: 100 ohm series resistors, 100 nF differential / 10 nF to ground.
constexpr uint8_t CR0_OPEN_CIRCUIT = BIT(4);
constexpr uint8_t CR0_INTERRUPT_FAULT = BIT(2);
constexpr uint8_t CR0_FAULT_CLEAR = BIT(1);
// 60 Hz rejection, internal CJ sensor, comparator faults (automatic recovery).
constexpr uint8_t CR0_CONFIG = CR0_AUTOMATIC | CR0_OPEN_CIRCUIT;

} // namespace

Thermocouples::Thermocouples() = default;

int Thermocouples::transfer(uint8_t channel, uint8_t* tx, uint8_t* rx, size_t length)
{
    if (channel >= CHANNEL_COUNT) {
        return -EINVAL;
    }
    return spi_decoder::transfer(bsp::thermocouple_decoder_addresses[channel],
                                 bsp::thermocouple_spi_config, tx, rx, length);
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

bool Thermocouples::channel_enabled(size_t channel) const
{
    return channel < CHANNEL_COUNT && (enabled_mask_ & (1U << channel)) != 0;
}

uint8_t Thermocouples::enabled_mask() const
{
    return enabled_mask_;
}

int Thermocouples::init(const ThermocoupleConfig& config)
{
    if (init_attempted_) {
        return -EALREADY;
    }
    init_attempted_ = true;

    // Validate the entire request before any GPIO or SPI operations.
    uint8_t mask = 0;
    for (size_t ch = 0; ch < CHANNEL_COUNT; ++ch) {
        const auto& settings = config.channels[ch];
        if (static_cast<uint8_t>(settings.type) >
            static_cast<uint8_t>(ThermocoupleType::T)) {
            return -EINVAL;
        }
        if (settings.enabled) {
            mask |= static_cast<uint8_t>(1U << ch);
        }
    }
    config_ = config;
    enabled_mask_ = mask;

    if (mask == 0) {
        initialized_ = true;
        LOG_INF("All thermocouple channels disabled by configuration");
        return 0;
    }

    int ret = spi_decoder::initialize();
    if (ret < 0) {
        return ret;
    }

    // Configure each amplifier independently.
    for (uint8_t ch = 0; ch < CHANNEL_COUNT; ++ch) {
        if (!channel_enabled(ch)) {
            LOG_INF("TC%u disabled; skipping configuration", static_cast<unsigned>(ch + 1));
            continue;
        }

        // An MCU reset need not reset the powered MAX31856. Stop conversions
        // and clear retained faults in interrupt mode, where FAULTCLR applies.
        // This cannot remove an electrical OVUV condition on the analog inputs.
        ret = write_register(ch, REG_CR0, CR0_INTERRUPT_FAULT | CR0_FAULT_CLEAR);
        if (ret < 0) {
            return ret;
        }

        // Type occupies CR1 bits 3:0; AVGSEL remains zero (one sample).
        const uint8_t expected_cr1 = static_cast<uint8_t>(config_.channels[ch].type);
        ret = write_register(ch, REG_CR1, expected_cr1);

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

        if (cr0 != CR0_CONFIG || cr1 != expected_cr1) {
            LOG_ERR(
                "TC%u configuration readback failed: CR0=0x%02X CR1=0x%02X",
                static_cast<unsigned>(ch + 1), static_cast<unsigned>(cr0),
                static_cast<unsigned>(cr1)
            );

            return -EIO;
        }

        LOG_INF(
            "TC%u MAX31856 configured: type %s",
            static_cast<unsigned>(ch + 1),
            thermocouple_type_name(config_.channels[ch].type)
        );
    }

    // Allow initial conversions to complete.
    k_msleep(200);

    for (uint8_t ch = 0; ch < CHANNEL_COUNT; ++ch) {
        if (!channel_enabled(ch)) {
            continue;
        }
        uint8_t fault = 0;
        ret = read_registers(ch, REG_FAULT_STATUS, &fault, 1);
        if (ret < 0) {
            LOG_WRN("TC%u startup fault read failed: %d", static_cast<unsigned>(ch + 1), ret);
        } else if (fault & FAULT_OVUV) {
            LOG_WRN("TC%u input OV/UV (fault 0x%02X): conversions suspended; check input bias/probe",
                    static_cast<unsigned>(ch + 1), static_cast<unsigned>(fault));
        } else if (fault != 0) {
            LOG_WRN("TC%u startup fault 0x%02X", static_cast<unsigned>(ch + 1),
                    static_cast<unsigned>(fault));
        }
    }

    initialized_ = true;

    LOG_INF("Enabled MAX31856 channels initialized (mask 0x%02X)",
            static_cast<unsigned>(enabled_mask()));

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
    if (!channel_enabled(channel)) {
        result.error = -ENODEV;
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

        if (channel_enabled(ch) && ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }

    return initialized_ ? first_error : -ENODEV;
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
    if (!channel_enabled(channel)) {
        return -ENODEV;
    }

    // OVUV suspends conversions, leaving the CJ register stale (often zero
    // after power-on). Do not report that register as a valid temperature.
    uint8_t fault = 0;
    int ret = read_registers(static_cast<uint8_t>(channel), REG_FAULT_STATUS, &fault, 1);
    if (ret < 0) {
        return ret;
    }
    if (fault & FAULT_OVUV) {
        return -EIO;
    }

    // MAX31856:
    // 0x0A = Cold-junction temperature MSB
    // 0x0B = Cold-junction temperature LSB
    constexpr uint8_t REG_CJ_TEMP = 0x0A;

    uint8_t data[2]{};

    ret = read_registers(
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
