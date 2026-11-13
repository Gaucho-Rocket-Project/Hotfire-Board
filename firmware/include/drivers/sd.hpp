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


enum class SampleType : uint8_t { Temperature, Pressure };

struct SensorSample {
    SampleType type = SampleType::Temperature;
    uint64_t uptime_us = 0;
    uint32_t acquisition_us = 0;
    int64_t uptime_ms = 0;
    char timestamp_utc[21] = "UNSET";

    // MAX31856 internal cold-junction temperatures
    int32_t cold_junction_mc[4]{};

    // MAX31856 external thermocouple temperatures
    int32_t temperature_mc[4]{};

    // Raw MAX31856 fault status registers
    uint8_t tc_fault[4]{};

    // Each bit represents one valid measurement
    uint8_t cj_valid_mask = 0;
    uint8_t tc_valid_mask = 0;
    uint8_t fault_valid_mask = 0;

    // PC0..PC3 (P0..P3): 12-bit ADC counts and measured loop current.
    uint16_t pressure_raw[4]{};
    int32_t pressure_ua[4]{};
    uint8_t pressure_valid_mask = 0;
};


struct BufferStatus {
    bool mounted;
    size_t pending_events;
    size_t pending_samples;
    uint32_t dropped_events;
    uint32_t dropped_samples;
    char session_directory[40];
    uint64_t written_samples;
    uint32_t write_errors;
    int last_write_error;
    uint32_t last_batch_us;
    uint32_t max_batch_us;
};

int mount_sd();
int unmount_sd();

// Returns:
//  1 = card inserted
//  0 = no card
// <0 = GPIO error
int card_present();

int flush_pending();

BufferStatus get_status();

int log_status(LogLevel level, const char* message);
int append_startup_record();
// Nonblocking RAM enqueue only; -ENOSPC drops the new record if the queue is full.
int append_sensor_sample(const SensorSample& sample);

}
#endif // DRIVERS_SD_H
