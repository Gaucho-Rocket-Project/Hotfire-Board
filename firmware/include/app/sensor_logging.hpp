#ifndef APP_SENSOR_LOGGING_H
#define APP_SENSOR_LOGGING_H

#include <stdint.h>

class Thermocouples;
class PressureTransducers;

namespace sensor_logging {

struct Stats {
    bool running = false;
    bool stopping = false;

    uint32_t samples = 0;
    uint32_t storage_errors = 0;

    int last_storage_error = 0;
    uint32_t pressure_errors = 0;
    int last_pressure_error = 0;
};

// Create the background thread and bind the driver.
// Call once after attempting to initialize both drivers. Failed channels
// remain blank while working sensors continue logging.
int initialize(Thermocouples& thermocouples, PressureTransducers& pressure);

// Start 1 Hz sensor acquisition.
int start();

// Stop acquisition and wait for outstanding writes.
// If SD is mounted, flush pending records.
int stop();

// True while logging or stopping is in progress.
bool active();

Stats get_stats();

} // sensor_logging

#endif // APP_SENSOR_LOGGING_H
