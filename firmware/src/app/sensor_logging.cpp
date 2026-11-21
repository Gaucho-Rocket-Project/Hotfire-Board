#include "app/sensor_logging.hpp"
#include "drivers/thermocouples.hpp"
#include "drivers/pressure.hpp"
#include "drivers/adxl345.hpp"
#include "drivers/sd.hpp"
#include "drivers/rtc.hpp"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <string.h>

namespace {

constexpr uint32_t TEMPERATURE_PERIOD_US = 100000;
constexpr uint32_t PRESSURE_PERIOD_US = 1000;
constexpr uint32_t ACCELEROMETER_PERIOD_US = 1000000 / ADXL345::RATE_HZ;
static_assert(CONFIG_SYS_CLOCK_TICKS_PER_SEC >= 1000,
              "1 kHz pressure acquisition needs at least 1000 kernel ticks/second");

enum Mode { Stopped, Running, Stopping };
atomic_t mode = Stopped;
K_MUTEX_DEFINE(control_mutex);
k_spinlock stats_lock{};
sensor_logging::StreamStats temperature_stats;
sensor_logging::StreamStats pressure_stats;
sensor_logging::StreamStats accelerometer_stats;
uint64_t accelerometer_fifo_overruns = 0;
uint64_t accelerometer_empty_polls = 0;
ADXL345::Reading last_acceleration{};
bool last_acceleration_valid = false;
uint64_t last_acceleration_us = 0;
bool accelerometer_started = false;

K_TIMER_DEFINE(temperature_timer, nullptr, nullptr);
K_TIMER_DEFINE(pressure_timer, nullptr, nullptr);
K_TIMER_DEFINE(accelerometer_timer, nullptr, nullptr);
K_SEM_DEFINE(temperature_start, 0, 1);
K_SEM_DEFINE(pressure_start, 0, 1);
K_SEM_DEFINE(accelerometer_start, 0, 1);
K_SEM_DEFINE(workers_stopped, 0, 3);
unsigned workers_pending = 0;

K_THREAD_STACK_DEFINE(temperature_stack, 2048);
K_THREAD_STACK_DEFINE(pressure_stack, 2048);
k_thread temperature_thread;
k_thread pressure_thread;
K_THREAD_STACK_DEFINE(accelerometer_stack, 2048);
k_thread accelerometer_thread;
ADXL345* accelerometer_driver = nullptr;
Thermocouples* tc_driver = nullptr;
PressureTransducers* pressure_driver = nullptr;

struct ReadResult {
    unsigned errors = 0;
    int last_error = 0;
    bool valid = false;
};

ReadResult acquire_temperature(storage::SensorSample& sample)
{
    ReadResult result;
    for (size_t ch = 0; ch < Thermocouples::CHANNEL_COUNT; ++ch) {
        if (!tc_driver->channel_enabled(ch)) {
            continue;
        }
        const uint8_t bit = static_cast<uint8_t>(1U << ch);
        int ret = tc_driver->read_cold_junction(ch, sample.cold_junction_mc[ch]);
        if (ret == 0) {
            sample.cj_valid_mask |= bit;
        } else {
            ++result.errors;
            result.last_error = ret;
        }
        Thermocouples::Reading reading{};
        ret = tc_driver->read(ch, reading);
        sample.tc_fault[ch] = reading.fault;
        if (ret == 0 || reading.fault != 0) {
            sample.fault_valid_mask |= bit;
        }
        if (ret == 0 && reading.valid) {
            sample.temperature_mc[ch] = reading.temperature_mc;
            sample.tc_valid_mask |= bit;
        } else if (ret < 0) {
            ++result.errors;
            result.last_error = ret;
        }
    }
    const uint8_t enabled = tc_driver->enabled_mask();
    result.valid = enabled != 0 && sample.cj_valid_mask == enabled &&
                   sample.tc_valid_mask == enabled;
    return result;
}

ReadResult acquire_pressure(storage::SensorSample& sample)
{
    PressureTransducers::Reading readings[PressureTransducers::CHANNEL_COUNT]{};
    ReadResult result;
    const int ret = pressure_driver->read_all(readings, sample.pressure_valid_mask);
    for (size_t ch = 0; ch < PressureTransducers::CHANNEL_COUNT; ++ch) {
        if (sample.pressure_valid_mask & (1U << ch)) {
            sample.pressure_raw[ch] = readings[ch].raw;
            sample.pressure_ua[ch] = readings[ch].current_ua;
        } else {
            ++result.errors;
        }
    }
    result.last_error = ret;
    result.valid = sample.pressure_valid_mask == 0x0f;
    return result;
}

void sampling_thread(void* argument, void*, void*)
{
    const bool pressure = argument != nullptr;
    k_timer* timer = pressure ? &pressure_timer : &temperature_timer;
    k_sem* start_signal = pressure ? &pressure_start : &temperature_start;
    const uint32_t period_us = pressure ? PRESSURE_PERIOD_US : TEMPERATURE_PERIOD_US;
    auto& stats = pressure ? pressure_stats : temperature_stats;

    while (true) {
        k_sem_take(start_signal, K_FOREVER);
        while (atomic_get(&mode) == Running) {
            // Periodic timer deadlines do not drift with acquisition or SD time.
            // Coalesced expirations count as missed slots; never fabricate catch-up samples.
            const uint32_t expirations = k_timer_status_sync(timer);
            if (expirations == 0 || atomic_get(&mode) != Running) {
                break;
            }

            storage::SensorSample sample{};
            sample.type = pressure ? storage::SampleType::Pressure : storage::SampleType::Temperature;
            const uint32_t start = k_cycle_get_32();
            sample.uptime_us = k_ticks_to_us_floor64(k_uptime_ticks());
            sample.uptime_ms = sample.uptime_us / 1000;
            const auto timestamp = rtc_clock::now();
            memcpy(sample.timestamp_utc, timestamp.utc, sizeof(sample.timestamp_utc));

            const ReadResult result = pressure ? acquire_pressure(sample) : acquire_temperature(sample);
            const uint32_t acquisition_cycles = k_cycle_get_32() - start;
            sample.acquisition_us = static_cast<uint32_t>(k_cyc_to_us_floor64(acquisition_cycles));
            const int ret = storage::SDCard::instance().enqueue(sample);
            const uint32_t total_cycles = k_cycle_get_32() - start;
            const uint32_t total_us = static_cast<uint32_t>(k_cyc_to_us_floor64(total_cycles));

            const auto key = k_spin_lock(&stats_lock);
            stats.record(sample.uptime_us, sample.acquisition_us, total_us,
                         expirations, period_us, result.valid, result.errors,
                         result.last_error, ret == 0);
            k_spin_unlock(&stats_lock, key);
        }
        k_sem_give(&workers_stopped);
    }
}


void acceleration_thread(void*, void*, void*)
{
    while (true) {
        k_sem_take(&accelerometer_start, K_FOREVER);
        while (atomic_get(&mode) == Running) {
            const uint32_t expirations = k_timer_status_sync(&accelerometer_timer);
            if (expirations == 0 || atomic_get(&mode) != Running) {
                break;
            }
            auto key = k_spin_lock(&stats_lock);
            accelerometer_stats.missed_periods += expirations - 1;
            k_spin_unlock(&stats_lock, key);

            // Drain at most the hardware FIFO depth per wake. Pressure runs at
            // a higher priority and can preempt every part of this worker.
            for (unsigned i = 0; i < ADXL345::FIFO_DEPTH &&
                                 atomic_get(&mode) == Running; ++i) {
                const uint32_t start = k_cycle_get_32();
                storage::SensorSample sample{};
                sample.type = storage::SampleType::Acceleration;
                // Host FIFO-read time, not an invented sensor conversion time.
                sample.uptime_us = k_ticks_to_us_floor64(k_uptime_ticks());
                sample.uptime_ms = sample.uptime_us / 1000;
                const auto timestamp = rtc_clock::now();
                memcpy(sample.timestamp_utc, timestamp.utc, sizeof(sample.timestamp_utc));
                ADXL345::Reading reading{};
                const int read_ret = accelerometer_driver->read(reading);
                if (read_ret < 0) {
                    key = k_spin_lock(&stats_lock);
                    if (read_ret == -EAGAIN) {
                        accelerometer_empty_polls += i == 0;
                    } else {
                        ++accelerometer_stats.read_errors;
                        accelerometer_stats.last_error = read_ret;
                    }
                    k_spin_unlock(&stats_lock, key);
                    break; // No duplicate or fabricated row for an empty/failed read.
                }
                memcpy(sample.acceleration_raw, reading.raw, sizeof(reading.raw));
                memcpy(sample.acceleration_mg, reading.mg, sizeof(reading.mg));
                sample.acceleration_valid = true;
                sample.acceleration_fifo_overrun = reading.fifo_overrun;
                sample.acquisition_us = static_cast<uint32_t>(
                    k_cyc_to_us_floor64(k_cycle_get_32() - start));
                const int ret = storage::SDCard::instance().enqueue(sample);
                const uint32_t total_us = static_cast<uint32_t>(
                    k_cyc_to_us_floor64(k_cycle_get_32() - start));
                key = k_spin_lock(&stats_lock);
                accelerometer_stats.record(sample.uptime_us, sample.acquisition_us,
                    total_us, 1, ACCELEROMETER_PERIOD_US, true, 0, 0, ret == 0);
                accelerometer_fifo_overruns += reading.fifo_overrun;
                last_acceleration = reading;
                last_acceleration_valid = true;
                last_acceleration_us = sample.uptime_us;
                k_spin_unlock(&stats_lock, key);
            }
        }
        k_sem_give(&workers_stopped);
    }
}

} // namespace

namespace sensor_logging {

int initialize(Thermocouples& thermocouples, PressureTransducers& pressure, ADXL345& accelerometer)
{
    k_mutex_lock(&control_mutex, K_FOREVER);
    if (tc_driver != nullptr) {
        k_mutex_unlock(&control_mutex);
        return -EALREADY;
    }
    tc_driver = &thermocouples;
    pressure_driver = &pressure;
    const auto bind_key = k_spin_lock(&stats_lock);
    accelerometer_driver = &accelerometer;
    k_spin_unlock(&stats_lock, bind_key);
    k_thread_create(&temperature_thread, temperature_stack, K_THREAD_STACK_SIZEOF(temperature_stack),
                    sampling_thread, nullptr, nullptr, nullptr, 5, 0, K_NO_WAIT);
    k_thread_create(&pressure_thread, pressure_stack, K_THREAD_STACK_SIZEOF(pressure_stack),
                    sampling_thread, &pressure_thread, nullptr, nullptr, 1, 0, K_NO_WAIT);
    k_thread_create(&accelerometer_thread, accelerometer_stack,
                    K_THREAD_STACK_SIZEOF(accelerometer_stack),
                    acceleration_thread, nullptr, nullptr, nullptr, 3, 0, K_NO_WAIT);
    k_thread_name_set(&accelerometer_thread, "accelerometer");
    k_thread_name_set(&temperature_thread, "temperature");
    k_thread_name_set(&pressure_thread, "pressure");
    k_mutex_unlock(&control_mutex);
    return 0;
}

int start()
{
    k_mutex_lock(&control_mutex, K_FOREVER);
    if (tc_driver == nullptr) {
        k_mutex_unlock(&control_mutex);
        return -ENODEV;
    }
    if (atomic_get(&mode) != Stopped) {
        k_mutex_unlock(&control_mutex);
        return -EALREADY;
    }
    // Reset FIFO before waking acquisition; never record data from the idle period.
    const int accel_ret = accelerometer_driver->ready() ? accelerometer_driver->clear_fifo() : -ENODEV;
    const auto key = k_spin_lock(&stats_lock);
    accelerometer_started = accel_ret == 0;
    accelerometer_stats = {};
    accelerometer_fifo_overruns = 0;
    accelerometer_empty_polls = 0;
    last_acceleration_valid = false;
    last_acceleration = {};
    last_acceleration_us = 0;
    if (accel_ret < 0 && accelerometer_driver->ready()) {
        accelerometer_stats.read_errors = 1;
        accelerometer_stats.last_error = accel_ret;
    }
    temperature_stats = {};
    pressure_stats = {};
    k_spin_unlock(&stats_lock, key);
    k_sem_reset(&workers_stopped);
    workers_pending = accelerometer_started ? 3 : 2;
    atomic_set(&mode, Running);
    k_timer_start(&temperature_timer, K_NO_WAIT, K_USEC(TEMPERATURE_PERIOD_US));
    k_timer_start(&pressure_timer, K_NO_WAIT, K_USEC(PRESSURE_PERIOD_US));
    if (accelerometer_started) {
        k_timer_start(&accelerometer_timer, K_USEC(ACCELEROMETER_PERIOD_US),
                      K_USEC(ACCELEROMETER_PERIOD_US));
        k_sem_give(&accelerometer_start);
    }
    k_sem_give(&temperature_start);
    k_sem_give(&pressure_start);
    k_mutex_unlock(&control_mutex);
    return 0;
}

int stop()
{
    // Workers never acquire this mutex. Serialize start/stop through draining.
    k_mutex_lock(&control_mutex, K_FOREVER);
    if (atomic_get(&mode) != Stopped) {
        atomic_set(&mode, Stopping);
        k_timer_stop(&temperature_timer);
        k_timer_stop(&pressure_timer);
        k_timer_stop(&accelerometer_timer);
        while (workers_pending) {
            const int ret = k_sem_take(&workers_stopped, K_SECONDS(10));
            if (ret < 0) {
                // Keep Stopping and pending acknowledgments so a later stop can retry.
                k_mutex_unlock(&control_mutex);
                return ret;
            }
            --workers_pending;
        }
        atomic_set(&mode, Stopped);
    }
    const int ret = storage::SDCard::instance().flush();
    k_mutex_unlock(&control_mutex);
    return ret == -ENODEV ? 0 : ret;
}

bool active()
{
    return atomic_get(&mode) != Stopped;
}

Stats get_stats()
{
    Stats stats;
    const auto state = atomic_get(&mode);
    stats.running = state == Running;
    stats.stopping = state == Stopping;
    const auto key = k_spin_lock(&stats_lock);
    stats.temperature = temperature_stats;
    stats.pressure = pressure_stats;
    stats.accelerometer = accelerometer_stats;
    stats.accelerometer_available = accelerometer_driver != nullptr && accelerometer_driver->ready();
    stats.accelerometer_running = accelerometer_started && state == Running;
    stats.accelerometer_fifo_overruns = accelerometer_fifo_overruns;
    stats.accelerometer_empty_polls = accelerometer_empty_polls;
    stats.acceleration_sample_valid = last_acceleration_valid;
    memcpy(stats.acceleration_raw, last_acceleration.raw, sizeof(last_acceleration.raw));
    memcpy(stats.acceleration_mg, last_acceleration.mg, sizeof(last_acceleration.mg));
    stats.acceleration_capture_us = last_acceleration_us;
    k_spin_unlock(&stats_lock, key);
    return stats;
}

} // namespace sensor_logging
