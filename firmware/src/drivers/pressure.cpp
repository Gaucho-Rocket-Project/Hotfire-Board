#include "drivers/pressure.hpp"
#include "drivers/utils/pressure_conversion.hpp"

#include <zephyr/drivers/adc.h>
#include <errno.h>

namespace {

constexpr adc_dt_spec inputs[] = {
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0),
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1),
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 2),
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 3),
};
static_assert(ARRAY_SIZE(inputs) == PressureTransducers::CHANNEL_COUNT);

} // namespace

int PressureTransducers::init()
{
    int first_error = 0;
    for (size_t i = 0; i < CHANNEL_COUNT; ++i) {
        int ret = adc_is_ready_dt(&inputs[i]) ? adc_channel_setup_dt(&inputs[i]) : -ENODEV;
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

    const auto& input = inputs[channel];
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
