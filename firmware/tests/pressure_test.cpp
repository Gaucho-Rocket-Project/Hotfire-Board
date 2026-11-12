// From firmware/:
// c++ -std=c++17 -Wall -Wextra -Werror -Itests/pressure/fakes -Iinclude
// tests/pressure_test.cpp src/drivers/pressure.cpp -o /tmp/pressure_test
// /tmp/pressure_test
#include "drivers/pressure.hpp"
#include <zephyr/drivers/adc.h>

#include <cassert>
#include <errno.h>

namespace {
bool device_ready = true;
int setup_errors[4]{};
int read_errors[4]{};
int conversion_error = 0;
unsigned setup_calls[4]{};
unsigned read_calls[4]{};
uint16_t raw_values[4] = {645, 1936, 3227, 0}; // Approximately 4, 12, 20, 0 mA.
}

bool adc_is_ready_dt(const adc_dt_spec*) { return device_ready; }

int adc_channel_setup_dt(const adc_dt_spec* spec)
{
    ++setup_calls[spec->index];
    return setup_errors[spec->index];
}

int adc_sequence_init_dt(const adc_dt_spec* spec, adc_sequence* sequence)
{
    sequence->index = spec->index;
    return 0;
}

int adc_read_dt(const adc_dt_spec* spec, const adc_sequence* sequence)
{
    assert(sequence->index == spec->index);
    assert(sequence->buffer_size == sizeof(uint16_t));
    ++read_calls[spec->index];
    *static_cast<uint16_t*>(sequence->buffer) = raw_values[spec->index];
    return read_errors[spec->index];
}

int adc_raw_to_microvolts_dt(const adc_dt_spec*, int32_t* value)
{
    if (conversion_error) {
        return conversion_error;
    }
    *value = static_cast<int32_t>(static_cast<int64_t>(*value) * 3300000 / 4096);
    return 0;
}

int main()
{
    PressureTransducers driver;
    PressureTransducers::Reading reading{123, 456};
    assert(driver.read(0, reading) == -ENODEV);
    assert(reading.raw == 0 && reading.current_ua == 0);
    assert(driver.init() == 0);

    const int expected[] = {4000, 12000, 20000, 0};
    for (size_t i = 0; i < 4; ++i) {
        assert(setup_calls[i] == 1);
        assert(driver.read(i, reading) == 0);
        assert(reading.raw == raw_values[i]);
        assert(reading.current_ua >= expected[i] - 7);
        assert(reading.current_ua <= expected[i] + 7);
    }
    assert(reading.current_ua == 0); // A genuine zero is a successful measurement.

    raw_values[0] = 4095;
    assert(driver.read(0, reading) == 0);
    assert(reading.current_ua > 25000); // Do not clamp an over-range loop to 20 mA.

    read_errors[1] = -EIO;
    assert(driver.read(1, reading) == -EIO);
    assert(reading.raw == 0 && reading.current_ua == 0); // Never return stale data.
    assert(driver.read(2, reading) == 0);
    conversion_error = -ENOTSUP;
    assert(driver.read(2, reading) == -ENOTSUP);
    assert(reading.raw == 0 && reading.current_ua == 0);
    conversion_error = 0;
    assert(driver.read(4, reading) == -EINVAL);

    // Failed setup on one channel must not prevent configuring the other three.
    setup_errors[1] = -EINVAL;
    PressureTransducers partial;
    assert(partial.init() == -EINVAL);
    for (size_t i = 0; i < 4; ++i) {
        assert(setup_calls[i] == 2);
    }
    const unsigned before = read_calls[1];
    assert(partial.read(1, reading) == -ENODEV);
    assert(read_calls[1] == before);
    assert(partial.read(2, reading) == 0);

    device_ready = false;
    PressureTransducers absent;
    assert(absent.init() == -ENODEV);
    assert(absent.read(0, reading) == -ENODEV);
}
