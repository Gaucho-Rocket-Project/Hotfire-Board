#include "drivers/adxl345.hpp"
#include "drivers/spi_decoder.hpp"
#include "bsp.hpp"

#include <errno.h>
#include <string.h>

namespace {
constexpr uint8_t DEVID = 0x00;
constexpr uint8_t BW_RATE = 0x2c;
constexpr uint8_t POWER_CTL = 0x2d;
constexpr uint8_t INT_ENABLE = 0x2e;
constexpr uint8_t INT_SOURCE = 0x30;
constexpr uint8_t DATA_FORMAT = 0x31;
constexpr uint8_t DATAX0 = 0x32;
constexpr uint8_t FIFO_CTL = 0x38;
constexpr uint8_t FIFO_STATUS = 0x39;
static_assert(ADXL345::RATE_HZ == 1600, "Update BW_RATE when changing the ODR");
constexpr uint8_t STREAM = 0x81; // Stream mode, watermark = one sample.
}

ADXL345::ADXL345()
{
    k_mutex_init(&mutex_);
}

int ADXL345::write_register(uint8_t address, uint8_t value)
{
    uint8_t tx[2]{address, value};
    uint8_t rx[2]{};
    return spi_decoder::transfer(bsp::accelerometer_decoder_address,
                                 bsp::accelerometer_spi_config, tx, rx, sizeof(tx));
}

int ADXL345::read_registers(uint8_t address, uint8_t* data, size_t length)
{
    if (data == nullptr || length == 0 || length > 6) {
        return -EINVAL;
    }
    uint8_t tx[7]{};
    uint8_t rx[7]{};
    tx[0] = address | 0x80U | (length > 1 ? 0x40U : 0);
    const int ret = spi_decoder::transfer(bsp::accelerometer_decoder_address,
                                         bsp::accelerometer_spi_config, tx, rx, length + 1);
    if (ret == 0) {
        memcpy(data, rx + 1, length);
    }
    return ret;
}

int ADXL345::init()
{
    if (init_attempted_) {
        return -EALREADY;
    }
    init_attempted_ = true;
    int ret = spi_decoder::initialize();
    uint8_t id = 0;
    if (ret == 0) {
        ret = read_registers(DEVID, &id, 1);
    }
    if (ret < 0 || id != 0xe5) {
        return ret < 0 ? ret : -ENODEV;
    }

    // Stop measurement first: an MCU reset does not reset the sensor.
    const uint8_t settings[][2] = {
        {POWER_CTL, 0x00}, {INT_ENABLE, 0x00}, {FIFO_CTL, 0x00},
        {BW_RATE, 0x0e},       // 1600 Hz, normal power; nominal 800 Hz bandwidth.
        {DATA_FORMAT, 0x0b},   // Full resolution, +/-16 g, right justified, 4-wire.
        {FIFO_CTL, STREAM}, {POWER_CTL, 0x08}
    };
    for (const auto& setting : settings) {
        ret = write_register(setting[0], setting[1]);
        uint8_t actual = 0;
        if (ret == 0) {
            ret = read_registers(setting[0], &actual, 1);
        }
        if (ret < 0 || actual != setting[1]) {
            (void)write_register(POWER_CTL, 0x00);
            return ret < 0 ? ret : -EIO;
        }
    }
    initialized_ = true;
    return 0;
}

int ADXL345::clear_fifo()
{
    if (!initialized_) {
        return -ENODEV;
    }
    k_mutex_lock(&mutex_, K_FOREVER);
    int ret = write_register(FIFO_CTL, 0x00);
    if (ret == 0) {
        ret = write_register(FIFO_CTL, STREAM);
    }
    k_mutex_unlock(&mutex_);
    return ret;
}

int ADXL345::read(Reading& reading)
{
    reading = {};
    if (!initialized_) {
        return -ENODEV;
    }
    k_mutex_lock(&mutex_, K_FOREVER);
    const int ret = [&]() -> int {
        uint8_t entries = 0;
        int result = read_registers(FIFO_STATUS, &entries, 1);
        if (result < 0) {
            return result;
        }
        entries &= 0x3f;
        if (entries == 0) {
            return -EAGAIN;
        }
        if (entries > FIFO_DEPTH) {
            return -EIO;
        }
        uint8_t source = 0;
        result = read_registers(INT_SOURCE, &source, 1);
        if (result < 0) {
            return result;
        }
        uint8_t data[6]{};
        result = read_registers(DATAX0, data, sizeof(data));
        // The FIFO needs at least 5 us after a pop before the next FIFO access.
        k_busy_wait(5);
        if (result < 0) {
            return result;
        }
        Reading candidate{};
        candidate.fifo_overrun = (source & 0x01U) != 0;
        for (size_t axis = 0; axis < 3; ++axis) {
            int32_t value = data[axis * 2] | (uint32_t(data[axis * 2 + 1]) << 8);
            if (value & 0x8000) {
                value -= 0x10000;
            }
            candidate.raw[axis] = static_cast<int16_t>(value);
            candidate.mg[axis] = value * 4;
        }
        reading = candidate;
        return 0;
    }();
    k_mutex_unlock(&mutex_);
    return ret;
}
