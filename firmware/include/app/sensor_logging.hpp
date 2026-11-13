#ifndef APP_SENSOR_LOGGING_H
#define APP_SENSOR_LOGGING_H

#include <stdint.h>
#include "app/sampling_diagnostics.hpp"

class Thermocouples;
class PressureTransducers;

namespace sensor_logging {

struct Stats {
    bool running = false;
    bool stopping = false;

    StreamStats temperature;
    StreamStats pressure;
};

// Create the background thread and bind the driver.
// Call once after attempting to initialize both drivers. Failed channels
// remain blank while working sensors continue logging.
int initialize(Thermocouples& thermocouples, PressureTransducers& pressure);

// Start independent 10 Hz temperature and 1000 Hz pressure acquisition.
int start();

// Stop acquisition and wait for outstanding writes.
// If SD is mounted, flush pending records.
int stop();

// True while logging or stopping is in progress.
bool active();

Stats get_stats();

} // sensor_logging

#endif // APP_SENSOR_LOGGING_H
