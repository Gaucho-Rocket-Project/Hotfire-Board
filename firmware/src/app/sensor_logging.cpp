#include "app/sensor_logging.hpp"
#include "drivers/thermocouples.hpp"
#include "drivers/pressure.hpp"
#include "drivers/sd.hpp"
#include "drivers/rtc.hpp"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <string.h>

namespace {

constexpr uint32_t TEMPERATURE_PERIOD_US = 100000;
constexpr uint32_t PRESSURE_PERIOD_US = 1000;
static_assert(CONFIG_SYS_CLOCK_TICKS_PER_SEC >= 1000,
              "1 kHz pressure acquisition needs at least 1000 kernel ticks/second");

enum Mode { Stopped, Running, Stopping };
atomic_t mode = Stopped;
K_MUTEX_DEFINE(control_mutex);
k_spinlock stats_lock{};
sensor_logging::StreamStats temperature_stats;
sensor_logging::StreamStats pressure_stats;

K_TIMER_DEFINE(temperature_timer, nullptr, nullptr);
K_TIMER_DEFINE(pressure_timer, nullptr, nullptr);
K_SEM_DEFINE(temperature_start, 0, 1);
K_SEM_DEFINE(pressure_start, 0, 1);
K_SEM_DEFINE(workers_stopped, 0, 2);
unsigned workers_pending = 0;

K_THREAD_STACK_DEFINE(temperature_stack, 2048);
K_THREAD_STACK_DEFINE(pressure_stack, 2048);
k_thread temperature_thread;
k_thread pressure_thread;
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
    result.valid = sample.cj_valid_mask == 0x0f && sample.tc_valid_mask == 0x0f;
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
            const int ret = storage::append_sensor_sample(sample);
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

} // namespace

namespace sensor_logging {

int initialize(Thermocouples& thermocouples, PressureTransducers& pressure)
{
    k_mutex_lock(&control_mutex, K_FOREVER);
    if (tc_driver != nullptr) {
        k_mutex_unlock(&control_mutex);
        return -EALREADY;
    }
    tc_driver = &thermocouples;
    pressure_driver = &pressure;
    k_thread_create(&temperature_thread, temperature_stack, K_THREAD_STACK_SIZEOF(temperature_stack),
                    sampling_thread, nullptr, nullptr, nullptr, 5, 0, K_NO_WAIT);
    k_thread_create(&pressure_thread, pressure_stack, K_THREAD_STACK_SIZEOF(pressure_stack),
                    sampling_thread, &pressure_thread, nullptr, nullptr, 1, 0, K_NO_WAIT);
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
    const auto key = k_spin_lock(&stats_lock);
    temperature_stats = {};
    pressure_stats = {};
    k_spin_unlock(&stats_lock, key);
    k_sem_reset(&workers_stopped);
    workers_pending = 2;
    atomic_set(&mode, Running);
    k_timer_start(&temperature_timer, K_NO_WAIT, K_USEC(TEMPERATURE_PERIOD_US));
    k_timer_start(&pressure_timer, K_NO_WAIT, K_USEC(PRESSURE_PERIOD_US));
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
    const int ret = storage::flush_pending();
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
    k_spin_unlock(&stats_lock, key);
    return stats;
}

} // namespace sensor_logging
