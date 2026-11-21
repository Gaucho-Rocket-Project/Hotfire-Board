#include "bsp.hpp"
#include "drivers/adxl345.hpp"
#include "drivers/spi_decoder.hpp"
#include <zephyr/drivers/spi.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <deque>
#include <array>
#include <thread>

namespace bsp {
const device* const sensor_spi = &fake_spi;
const spi_config thermocouple_spi_config = {1000000, SPI_MODE_CPHA | SPI_WORD_SET(8), 0};
const spi_config accelerometer_spi_config = {1000000, SPI_MODE_CPHA | SPI_MODE_CPOL | SPI_WORD_SET(8), 0};
const DecoderAddressPort spi_decoder_low = {&fake_gpioa, 5, 3, 7U << 5};
const gpio_dt_spec spi_decoder_a3 = {&fake_gpioc, 4, 0};
const uint8_t accelerometer_decoder_address = 4;
}

namespace {
unsigned address = 15;
const spi_config* configured = nullptr;
unsigned primes = 0;
uint8_t registers[64]{};
std::deque<std::array<int16_t, 3>> fifo;
bool overrun = false;
bool corrupt_format = false;
bool fail_transfer = false;
bool fail_select = false;
bool fail_prime = false;
}

int gpio_pin_set(const device* port, gpio_pin_t pin, int value)
{
    if (port == &fake_gpioc && value == 0 && fail_select) {
        fail_select = false;
        return -EIO;
    }
    const unsigned bit = port == &fake_gpioc ? 3 : pin - 5;
    address = (address & ~(1U << bit)) | (unsigned(value) << bit);
    return 0;
}
int gpio_pin_configure(const device* port, gpio_pin_t pin, int)
{
    return gpio_pin_set(port, pin, 1);
}
int gpio_port_set_masked_raw(const device* port, gpio_port_pins_t mask, gpio_port_value_t value)
{
    assert(port == &fake_gpioa && mask == (7U << 5));
    // Low address pins must never change with a populated device selected.
    assert(address & 8);
    address = (address & 8) | ((value >> 5) & 7);
    return 0;
}
int spi_transceive(const device*, const spi_config* config,
                   const spi_buf_set* tx, const spi_buf_set* rx)
{
    std::this_thread::yield(); // Increase exposure of missing cross-driver locking.
    if (address == 15) {
        assert(tx->buffers[0].len == 1);
        configured = config;
        ++primes;
        if (fail_prime) { fail_prime = false; return -EIO; }
        return 0;
    }
    assert(configured == config); // Mode must change before real chip select.
    if (fail_transfer) { fail_transfer = false; return -EIO; }
    if (address == 0) {
        assert(config == &bsp::thermocouple_spi_config);
        return 0;
    }
    assert(address == 4 && config == &bsp::accelerometer_spi_config);
    const auto* bytes = static_cast<const uint8_t*>(tx->buffers[0].buf);
    auto* result = static_cast<uint8_t*>(rx->buffers[0].buf);
    const unsigned length = tx->buffers[0].len;
    const unsigned reg = bytes[0] & 0x3f;
    memset(result, 0, length);
    if (!(bytes[0] & 0x80)) {
        assert(length == 2);
        registers[reg] = bytes[1];
        if (reg == 0x38 && bytes[1] == 0) { fifo.clear(); overrun = false; }
        return 0;
    }
    if (reg == 0x39) {
        result[1] = fifo.size();
    } else if (reg == 0x30) {
        result[1] = overrun ? 1 : 0;
    } else if (reg == 0x32) {
        assert(bytes[0] == 0xf2 && length == 7 && !fifo.empty());
        const auto sample = fifo.front();
        fifo.pop_front();
        overrun = false;
        for (unsigned i = 0; i < 3; ++i) {
            const uint16_t raw = static_cast<uint16_t>(sample[i]);
            result[1 + 2 * i] = raw & 0xff;
            result[2 + 2 * i] = raw >> 8;
        }
    } else {
        assert(length == 2);
        result[1] = registers[reg];
        if (reg == 0x31 && corrupt_format) { result[1] ^= 1; }
    }
    return 0;
}

int main()
{
    ADXL345 absent;
    assert(absent.init() == -ENODEV && !absent.ready());
    assert(address == 15);
    registers[0] = 0xe5;
    corrupt_format = true;
    ADXL345 bad;
    assert(bad.init() == -EIO && !bad.ready());
    assert(registers[0x2d] == 0); // Failed configuration returns to standby.
    corrupt_format = false;

    ADXL345 sensor;
    assert(sensor.init() == 0 && sensor.ready());
    assert(sensor.init() == -EALREADY);
    assert(registers[0x2c] == 0x0a && registers[0x31] == 0x0b);
    assert(registers[0x2d] == 8 && registers[0x38] == 0x81);
    ADXL345::Reading reading;
    assert(sensor.read(reading) == -EAGAIN);
    fifo.push_back({-256, 0, 256});
    fifo.push_back({-32768, 32767, -1});
    overrun = true;
    assert(sensor.read(reading) == 0 && reading.fifo_overrun);
    assert(reading.raw[0] == -256 && reading.mg[0] == -1024);
    assert(reading.mg[1] == 0 && reading.mg[2] == 1024);
    assert(sensor.read(reading) == 0 && !reading.fifo_overrun);
    assert(reading.mg[0] == -131072 && reading.mg[1] == 131068 && reading.mg[2] == -4);
    assert(sensor.read(reading) == -EAGAIN && reading.mg[0] == 0);
    fifo.push_back({1, 2, 3});
    assert(sensor.clear_fifo() == 0 && fifo.empty());

    uint8_t tx[2]{0x80, 0}, rx[2]{};
    assert(spi_decoder::transfer(8, bsp::thermocouple_spi_config, tx, rx, 2) == -EINVAL);
    fail_prime = true;
    assert(spi_decoder::transfer(0, bsp::thermocouple_spi_config, tx, rx, 2) == -EIO);
    assert(address == 15);
    fail_select = true;
    assert(spi_decoder::transfer(0, bsp::thermocouple_spi_config, tx, rx, 2) == -EIO);
    assert(address == 15);
    fail_transfer = true;
    assert(sensor.read(reading) == -EIO && address == 15);
    const unsigned before = primes;
    std::thread tc([&] {
        for (unsigned i = 0; i < 100; ++i) {
            assert(spi_decoder::transfer(0, bsp::thermocouple_spi_config, tx, rx, 2) == 0);
        }
    });
    std::thread accel([&] {
        for (unsigned i = 0; i < 100; ++i) { assert(sensor.read(reading) == -EAGAIN); }
    });
    tc.join();
    accel.join();
    assert(address == 15 && primes > before);
}
