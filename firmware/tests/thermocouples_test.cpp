#include "drivers/thermocouples.hpp"
#include "bsp.hpp"
#include <cassert>
#include <cerrno>
#include <cstring>

// Host board mappings replace src/bsp.cpp, which requires real devicetree nodes.
namespace bsp {
const device* const thermocouple_spi = &fake_spi;
const spi_config thermocouple_spi_config = {1000000, SPI_MODE_CPHA | SPI_WORD_SET(8), 0};
const DecoderAddressPort thermocouple_decoder_low = {&fake_gpioa, 5, 3, 7U << 5};
const gpio_dt_spec thermocouple_decoder_a3 = {&fake_gpioc, 4, 0};
}

namespace {
unsigned address = 15;
unsigned transfers[4]{};
uint8_t registers[4][16]{};
uint8_t input_fault[4]{};
bool cleared_faults[4]{};
}

int gpio_pin_configure(const device* port, gpio_pin_t pin, int)
{
    return gpio_pin_set(port, pin, 1);
}

int gpio_pin_set(const device* port, gpio_pin_t pin, int value)
{
    const unsigned bit = port == &fake_gpioc ? 3 : pin - 5;
    address = (address & ~(1U << bit)) | (static_cast<unsigned>(value) << bit);
    return 0;
}

int gpio_port_set_masked_raw(const device* port, gpio_port_pins_t mask,
                             gpio_port_value_t value)
{
    assert(port == &fake_gpioa && mask == (7U << 5));
    address = (address & 8U) | ((value >> 5) & 7U);
    return 0;
}

int spi_transceive(const device*, const spi_config*, const spi_buf_set* tx,
                   const spi_buf_set* rx)
{
    assert(address < 4);
    ++transfers[address];
    if (!Thermocouples::channel_enabled(address)) {
        return -EIO; // Disabled devices must never be accessed.
    }
    const auto* bytes = static_cast<const uint8_t*>(tx->buffers[0].buf);
    auto* result = static_cast<uint8_t*>(rx->buffers[0].buf);
    memset(result, 0, rx->buffers[0].len);
    const unsigned reg = bytes[0] & 0x7f;
    if (bytes[0] & 0x80) {
        if (reg == 0 && (bytes[1] & 0x02)) {
            assert((bytes[1] & 0x84) == 0x04); // Standby, interrupt fault mode.
            registers[address][0x0f] = 0;
            cleared_faults[address] = true;
            registers[address][reg] = bytes[1] & ~0x02; // FAULTCLR self-clears.
        } else {
            if (reg == 1) {
                assert(!(registers[address][0] & 0x80)); // Configure in standby.
            }
            registers[address][reg] = bytes[1];
        }
    } else {
        memcpy(result + 1, registers[address] + reg, tx->buffers[0].len - 1);
        if (reg <= 0x0f && reg + tx->buffers[0].len - 1 > 0x0f) {
            result[1 + 0x0f - reg] |= input_fault[address];
        }
    }
    return 0;
}

int main()
{
    static_assert(Thermocouples::ENABLED_CHANNEL_MASK != 0);
    unsigned first_enabled = 0;
    while (!Thermocouples::channel_enabled(first_enabled)) {
        ++first_enabled;
    }
    // Model an MCU reset while the amplifiers retain configuration/faults.
    for (auto& regs : registers) {
        regs[0] = 0x94;
        regs[0x0f] = 0x41;
    }
    input_fault[first_enabled] = 0x02; // A real OVUV condition survives clearing.
    Thermocouples driver;
    assert(driver.init() == 0);
    assert(address == 15);
    Thermocouples::Reading faulted;
    assert(driver.read(first_enabled, faulted) == -EIO);
    assert(!faulted.valid && faulted.fault == 0x02);
    int32_t cj = 123;
    assert(driver.read_cold_junction(first_enabled, cj) == -EIO && cj == 0);
    input_fault[first_enabled] = 0; // Input returns to its valid voltage range.
    for (unsigned ch = 0; ch < 4; ++ch) {
        if (!Thermocouples::channel_enabled(ch)) {
            Thermocouples::Reading disabled;
            assert(driver.read(ch, disabled) == -ENODEV && !disabled.valid);
            assert(driver.read_cold_junction(ch, cj) == -ENODEV);
            assert(transfers[ch] == 0 && !cleared_faults[ch]);
            continue;
        }
        assert(cleared_faults[ch]);
        assert(registers[ch][0x0f] == 0);
        assert(registers[ch][0] == 0x90); // Automatic + OC detection + comparator.
        registers[ch][0x0a] = 25; // Cold junction: 25 C.
        registers[ch][0x0c] = 1;  // Thermocouple: 25 C (3200 * 32).
        registers[ch][0x0d] = 0x90;
        Thermocouples::Reading reading;
        assert(driver.read(ch, reading) == 0);
        assert(reading.valid && reading.temperature_mc == 25000);
        int32_t cj = 0;
        assert(driver.read_cold_junction(ch, cj) == 0 && cj == 25000);
    }
    Thermocouples::Reading readings[4];
    assert(driver.read_all(readings) == 0);
    input_fault[first_enabled] = 0x41; // Open probe: faulted TC, usable CJ.
    assert(driver.read_all(readings) == -EIO);
    assert(!readings[first_enabled].valid && readings[first_enabled].fault == 0x41);
    assert(driver.read_cold_junction(first_enabled, cj) == 0 && cj == 25000);
    for (unsigned ch = 0; ch < 4; ++ch) {
        if (!Thermocouples::channel_enabled(ch)) {
            assert(!readings[ch].valid && readings[ch].error == -ENODEV);
            assert(transfers[ch] == 0);
        }
    }
    assert(address == 15);
}
