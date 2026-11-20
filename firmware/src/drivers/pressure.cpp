#include "drivers/pressure.hpp"
#include "bsp.hpp"
#include "drivers/utils/pressure_conversion.hpp"

#include <zephyr/drivers/adc.h>
#include <errno.h>

static_assert(bsp::pressure_channel_count == PressureTransducers::CHANNEL_COUNT);

int PressureTransducers::init()
{
    int first_error = 0;
    for (size_t i = 0; i < CHANNEL_COUNT; ++i) {
        const auto& input = bsp::pressure_inputs[i];
        int ret = adc_is_ready_dt(&input) ? adc_channel_setup_dt(&input) : -ENODEV;
        ready_[i] = ret == 0;
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }
    return first_error;
}

int PressureTransducers::read(size_t channel, Reading& reading) const
{
    reading = {};
    if (channel >= CHANNEL_COUNT) {
        return -EINVAL;
    }
    if (!ready_[channel]) {
        return -ENODEV;
    }

    const auto& input = bsp::pressure_inputs[channel];
    uint16_t raw = 0;
    adc_sequence sequence{};
    int ret = adc_sequence_init_dt(&input, &sequence);
    if (ret < 0) {
        return ret;
    }
    sequence.buffer = &raw;
    sequence.buffer_size = sizeof(raw);
    ret = adc_read_dt(&input, &sequence);
    if (ret < 0) {
        return ret;
    }

    int32_t voltage_uv = raw;
    ret = adc_raw_to_microvolts_dt(&input, &voltage_uv);
    if (ret < 0) {
        return ret;
    }
    reading.raw = raw;
    reading.current_ua = pressure_conversion::current_ua(voltage_uv);
    return 0;
}

int PressureTransducers::read_all(Reading (&readings)[CHANNEL_COUNT], uint8_t& valid_mask) const
{
    valid_mask = 0;
    uint16_t raw[CHANNEL_COUNT]{};
    adc_sequence sequence{};
    int ret = adc_sequence_init_dt(&bsp::pressure_inputs[0], &sequence);
    int first_error = 0;
    sequence.channels = 0;
    for (size_t i = 0; i < CHANNEL_COUNT; ++i) {
        readings[i] = {};
        if (ready_[i]) {
            sequence.channels |= 1U << bsp::pressure_inputs[i].channel_id;
        } else {
            first_error = -ENODEV;
        }
    }
    if (ret < 0 || sequence.channels == 0) {
        return ret < 0 ? ret : -ENODEV;
    }
    sequence.buffer = raw;
    sequence.buffer_size = sizeof(raw);
    // A single sequencer run samples every configured input, rather than
    // reconfiguring/restarting the ADC four times for each 1 ms frame.
    ret = adc_read_dt(&bsp::pressure_inputs[0], &sequence);
    if (ret < 0) {
        return ret;
    }
    size_t packed = 0;
    for (size_t i = 0; i < CHANNEL_COUNT; ++i) {
        if (!ready_[i]) {
            continue;
        }
        const uint16_t value = raw[packed++];
        int32_t uv = value;
        ret = adc_raw_to_microvolts_dt(&bsp::pressure_inputs[i], &uv);
        if (ret < 0) {
            if (first_error == 0) {
                first_error = ret;
            }
            continue;
        }
        readings[i].raw = value;
        readings[i].current_ua = pressure_conversion::current_ua(uv);
        valid_mask |= static_cast<uint8_t>(1U << i);
    }
    return first_error;
}
