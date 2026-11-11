#ifndef DRIVERS_SD_H
#define DRIVERS_SD_H

#include <stddef.h>
#include <stdint.h>

namespace storage {

enum class LogLevel {
    Info,
    Warning,
    Error
};

struct SensorSample {
    int64_t uptime_ms;
    int32_t temperature_mc[4];
    uint16_t pressure_raw[4];
};

struct BufferStatus {
    bool mounted;
    size_t pending_events;
    size_t pending_samples;
    uint32_t dropped_events;
    uint32_t dropped_samples;
};

int mount_sd();
int unmount_sd();

BufferStatus get_status();

int log_status(LogLevel level, const char* message);
int append_startup_record();
int append_sensor_sample(const SensorSample& sample);

}
#endif // DRIVERS_SD_H