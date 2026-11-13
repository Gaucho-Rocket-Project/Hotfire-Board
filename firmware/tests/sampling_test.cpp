#include "app/sampling_diagnostics.hpp"
#include "drivers/utils/sample_csv.hpp"

#include <cassert>
#include <string>
#include <vector>

std::vector<std::string> columns(const std::string& text)
{
    std::vector<std::string> result;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ',' || text[i] == '\r') {
            result.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return result;
}

int main()
{
    sensor_logging::StreamStats pressure;
    sensor_logging::StreamStats temperature;
    assert(pressure.rate_millihz() == 0);
    for (unsigned i = 0; i <= 1000; ++i) {
        pressure.record(i * 1000ULL, 150, 165, 1, 1000, true, 0, 0, true);
        if (i % 100 == 0) {
            temperature.record(i * 1000ULL, 250, 270, 1, 100000, true, 0, 0, true);
        }
    }
    assert(pressure.rate_millihz() == 1000000);
    assert(temperature.rate_millihz() == 10000);
    assert(pressure.acquisition_sum_us / pressure.samples == 150);
    assert(pressure.interval_us == 1000);
    // Scheduling stalls, sensor failure and queue overflow are distinct diagnostics.
    pressure.record(1005000, 1200, 1300, 5, 1000, false, 2, -EIO, false);
    assert(pressure.missed_periods == 4);
    assert(pressure.overruns == 1);
    assert(pressure.queue_drops == 1);
    assert(pressure.read_errors == 2 && pressure.last_error == -EIO);
    assert(pressure.valid_frames == 1001);
    assert(pressure.max_interval_us == 5000);
    assert(pressure.max_acquisition_us == 1200 && pressure.max_total_us == 1300);
    assert(pressure.rate_millihz() < 1000000);
    pressure = {};
    assert(pressure.samples == 0 && pressure.rate_millihz() == 0);

    char output[storage::CSV_LINE_SIZE];
    storage::SensorSample sample;
    sample.type = storage::SampleType::Pressure;
    sample.uptime_us = 1234567;
    sample.uptime_ms = 1234;
    sample.acquisition_us = 150;
    sample.pressure_valid_mask = 0x05;
    sample.pressure_raw[0] = 0; // Valid zero must be distinguishable from a failed channel.
    sample.pressure_raw[2] = 3227;
    sample.pressure_ua[2] = 20000;
    int n = storage::format_sensor_csv(sample, output, sizeof(output));
    assert(n > 0);
    auto header = columns(storage::CSV_HEADER);
    auto row = columns(std::string(output, n));
    assert(row.size() == header.size() && row.size() == 25);
    assert(row[2] == "pressure" && row[3] == "1234567" && row[4] == "150");
    for (unsigned i = 5; i < 17; ++i) {
        assert(row[i].empty()); // No stale temperature duplicated into pressure rows.
    }
    assert(row[17] == "0" && row[18].empty() && row[19] == "3227");
    assert(row[21] == "0" && row[22].empty() && row[23] == "20000");
    assert(storage::format_sensor_csv(sample, output, 4) == -ENOSPC);

    sample = {};
    sample.cj_valid_mask = 1;
    sample.tc_valid_mask = 1;
    sample.fault_valid_mask = 1;
    sample.cold_junction_mc[0] = -1234;
    sample.temperature_mc[0] = 45678;
    n = storage::format_sensor_csv(sample, output, sizeof(output));
    row = columns(std::string(output, n));
    assert(row[2] == "temperature" && row[5] == "-1234" && row[6] == "45678");
    assert(row[7] == "0x00");
    for (unsigned i = 17; i < 25; ++i) {
        assert(row[i].empty());
    }
}
