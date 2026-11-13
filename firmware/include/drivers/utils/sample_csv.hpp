#ifndef DRIVERS_UTILS_SAMPLE_CSV_HPP
#define DRIVERS_UTILS_SAMPLE_CSV_HPP

#include "drivers/sd.hpp"
#include <errno.h>
#include <stdio.h>

namespace storage {

inline constexpr char CSV_HEADER[] =
    "timestamp_utc,uptime_ms,sample_type,uptime_us,acquisition_us,"
    "tc1_cj_mc,tc1_mc,tc1_fault,tc2_cj_mc,tc2_mc,tc2_fault,"
    "tc3_cj_mc,tc3_mc,tc3_fault,tc4_cj_mc,tc4_mc,tc4_fault,"
    "p1_raw,p2_raw,p3_raw,p4_raw,p1_ua,p2_ua,p3_ua,p4_ua\r\n";
inline constexpr size_t CSV_LINE_SIZE = 384;

inline int format_sensor_csv(const SensorSample& sample, char* output, size_t capacity)
{
    size_t used = 0;
    int error = 0;
    auto append = [&](const char* format, auto... values) {
        if (error) {
            return;
        }
        const int n = snprintf(output + used, capacity - used, format, values...);
        if (n < 0) {
            error = -EIO;
        } else if (static_cast<size_t>(n) >= capacity - used) {
            error = -ENOSPC;
        } else {
            used += static_cast<size_t>(n);
        }
    };
    auto cell = [&](int64_t value, bool valid) {
        if (valid) {
            append(",%lld", static_cast<long long>(value));
        } else {
            append("%s", ",");
        }
    };
    append("%s,%lld,%s,%llu,%u", sample.timestamp_utc,
           static_cast<long long>(sample.uptime_ms),
           sample.type == SampleType::Pressure ? "pressure" : "temperature",
           static_cast<unsigned long long>(sample.uptime_us),
           static_cast<unsigned>(sample.acquisition_us));
    for (size_t i = 0; i < 4; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1U << i);
        cell(sample.cold_junction_mc[i], sample.cj_valid_mask & bit);
        cell(sample.temperature_mc[i], sample.tc_valid_mask & bit);
        if (sample.fault_valid_mask & bit) {
            append(",0x%02X", static_cast<unsigned>(sample.tc_fault[i]));
        } else {
            append("%s", ",");
        }
    }
    for (size_t i = 0; i < 4; ++i) {
        cell(sample.pressure_raw[i], sample.pressure_valid_mask & (1U << i));
    }
    for (size_t i = 0; i < 4; ++i) {
        cell(sample.pressure_ua[i], sample.pressure_valid_mask & (1U << i));
    }
    append("%s", "\r\n");
    return error ? error : static_cast<int>(used);
}

} // namespace storage
#endif
