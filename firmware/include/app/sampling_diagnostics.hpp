#ifndef APP_SAMPLING_DIAGNOSTICS_HPP
#define APP_SAMPLING_DIAGNOSTICS_HPP

#include <stdint.h>

namespace sensor_logging {

struct StreamStats {
    uint64_t samples = 0;
    uint64_t valid_frames = 0;
    uint64_t read_errors = 0;
    uint64_t missed_periods = 0;
    uint64_t overruns = 0;
    uint64_t queue_drops = 0;
    int last_error = 0;
    uint64_t first_start_us = 0;
    uint64_t last_start_us = 0;
    uint64_t interval_us = 0;
    uint64_t max_interval_us = 0;
    uint32_t acquisition_us = 0;
    uint32_t max_acquisition_us = 0;
    uint64_t acquisition_sum_us = 0;
    uint32_t total_us = 0;
    uint32_t max_total_us = 0;
    uint64_t total_sum_us = 0;

    uint64_t rate_millihz() const
    {
        const uint64_t elapsed = last_start_us - first_start_us;
        return samples > 1 && elapsed ? (samples - 1) * 1000000000ULL / elapsed : 0;
    }

    void record(uint64_t start_us, uint32_t acquisition, uint32_t total,
                uint32_t expirations, uint32_t period_us, bool valid,
                unsigned errors, int error, bool queued)
    {
        if (samples == 0) {
            first_start_us = start_us;
        } else {
            interval_us = start_us - last_start_us;
            if (interval_us > max_interval_us) {
                max_interval_us = interval_us;
            }
        }
        last_start_us = start_us;
        ++samples;
        valid_frames += valid;
        read_errors += errors;
        if (errors) {
            last_error = error;
        }
        missed_periods += expirations > 0 ? expirations - 1 : 0;
        overruns += total >= period_us;
        queue_drops += !queued;
        acquisition_us = acquisition;
        total_us = total;
        acquisition_sum_us += acquisition;
        total_sum_us += total;
        if (acquisition > max_acquisition_us) {
            max_acquisition_us = acquisition;
        }
        if (total > max_total_us) {
            max_total_us = total;
        }
    }
};

} // namespace sensor_logging
#endif
