#ifndef APP_SENSOR_LOGGING_H
#define APP_SENSOR_LOGGING_H

#include <stdint.h>
#include "app/sampling_diagnostics.hpp"

class Thermocouples;
class PressureTransducers;
class ADXL345;

namespace sensor_logging {

struct Stats {
    bool running = false;
    bool stopping = false;

    StreamStats temperature;
    StreamStats pressure;
    StreamStats accelerometer;
    bool accelerometer_available = false;
    bool accelerometer_running = false;
    uint64_t accelerometer_fifo_overruns = 0;
    uint64_t accelerometer_empty_polls = 0;
    bool acceleration_sample_valid = false;
    int16_t acceleration_raw[3]{};
    int32_t acceleration_mg[3]{};
    uint64_t acceleration_capture_us = 0;
};

// Create the background thread and bind the driver.
// Call once after attempting to initialize all three drivers. Failed channels
// remain blank while working sensors continue logging.
int initialize(Thermocouples& thermocouples, PressureTransducers& pressure, ADXL345& accelerometer);

// Start 10 Hz temperature, 1000 Hz pressure, and 100 Hz accelerometer FIFO polling.
int start();

// Stop acquisition and wait for outstanding writes.
// If SD is mounted, flush pending records.
int stop();

// True while logging or stopping is in progress.
bool active();

Stats get_stats();

} // sensor_logging

#endif // APP_SENSOR_LOGGING_H
