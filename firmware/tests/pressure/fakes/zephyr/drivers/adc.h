#pragma once

// Host-only ADC API stand-in. Production builds use Zephyr's real header.
#include <stddef.h>
#include <stdint.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ADC_DT_SPEC_GET_BY_IDX(node, index) {index}

struct adc_dt_spec { size_t index; };
struct adc_sequence {
    void* buffer = nullptr;
    size_t buffer_size = 0;
    size_t index = 0;
};

bool adc_is_ready_dt(const adc_dt_spec* spec);
int adc_channel_setup_dt(const adc_dt_spec* spec);
int adc_sequence_init_dt(const adc_dt_spec* spec, adc_sequence* sequence);
int adc_read_dt(const adc_dt_spec* spec, const adc_sequence* sequence);
int adc_raw_to_microvolts_dt(const adc_dt_spec* spec, int32_t* value);
