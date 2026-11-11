
#include "app/sensor_logging.hpp"

#include <zephyr/kernel.h>

#include <errno.h>
#include <stdint.h>

#include "drivers/thermocouples.hpp"
#include "drivers/sd.hpp"

namespace {

constexpr int32_t SAMPLE_PERIOD_MS = 1000;

enum class Mode {
    Stopped,
    Running,
    Stopping
};

// Synchronization
K_MUTEX_DEFINE(control_mutex);

K_SEM_DEFINE(start_signal, 0, 1);
K_SEM_DEFINE(wake_signal, 0, 1);
K_SEM_DEFINE(stopped_signal, 0, 1);

// Dedicated logger thread
K_THREAD_STACK_DEFINE(logger_stack, 2304);
struct k_thread logger_thread;

Thermocouples* tc_driver = nullptr;

Mode mode = Mode::Stopped;

uint32_t sample_count = 0;
uint32_t storage_error_count = 0;
int last_storage_error = 0;

// Read all four amplifiers into one sensor sample.
// The caller must ensure the driver is initialized.
void acquire_sample(Thermocouples& driver, storage::SensorSample& sample)
{
    sample.uptime_ms = k_uptime_get();

    for (size_t ch = 0; ch < Thermocouples::CHANNEL_COUNT; ch++) {

        const uint8_t bit = static_cast<uint8_t>(1U << ch);

        int32_t cj_mc = 0;

        int cj_ret = driver.read_cold_junction(
            ch,
            cj_mc
        );

        if (cj_ret == 0) {
            sample.cold_junction_mc[ch] = cj_mc;
            sample.cj_valid_mask |= bit;
        }

        Thermocouples::Reading reading{};

        int tc_ret = driver.read(ch, reading);

        sample.tc_fault[ch] = reading.fault;

        if (tc_ret == 0 || reading.fault != 0) {
            sample.fault_valid_mask |= bit;
        }

        if (tc_ret == 0 && reading.valid) {
            sample.temperature_mc[ch] = reading.temperature_mc;
            sample.tc_valid_mask |= bit;
        }
    }

    // Pressure data remains invalid until
    // ADC acquisition is implemented.
}

// Background thread entry point.
void logging_thread(void*, void*, void*)
{
    while (true) {

        (void)k_sem_take(
            &start_signal,
            K_FOREVER
        );

        while (true) {

            k_mutex_lock(
                &control_mutex,
                K_FOREVER
            );

            bool should_run = (mode == Mode::Running);
            Thermocouples* driver = tc_driver;

            k_mutex_unlock(&control_mutex);

            if (!should_run || driver == nullptr) {
                break;
            }

            int64_t cycle_start = k_uptime_get();

            storage::SensorSample sample{};

            acquire_sample(*driver, sample);

            // Buffered by the storage module.
            // Writes are synchronized if SD is mounted.
            int ret = storage::append_sensor_sample(sample);

            k_mutex_lock(
                &control_mutex,
                K_FOREVER
            );

            ++sample_count;

            if (ret < 0) {
                ++storage_error_count;
                last_storage_error = ret;
            }

            k_mutex_unlock(&control_mutex);

            // Wait until the next sampling interval.
            // The wake semaphore allows stop() to
            // interrupt the wait immediately.
            int64_t elapsed = k_uptime_get() - cycle_start;

            int64_t remaining = SAMPLE_PERIOD_MS - elapsed;

            if (remaining > 0) {
                (void)k_sem_take(
                    &wake_signal,
                    K_MSEC(static_cast<int32_t>(remaining))
                );
            }
        }

        k_mutex_lock(&control_mutex, K_FOREVER);
        mode = Mode::Stopped;
        k_mutex_unlock(&control_mutex);

        // Tell stop() the current operation is done.
        k_sem_give(&stopped_signal);
    }
}

} // namespace

namespace sensor_logging {

int initialize(Thermocouples& thermocouples)
{
    k_mutex_lock(
        &control_mutex,
        K_FOREVER
    );

    if (tc_driver != nullptr) {
        k_mutex_unlock(&control_mutex);
        return -EALREADY;
    }

    tc_driver = &thermocouples;

    (void)k_thread_create(
        &logger_thread,
        logger_stack,
        K_THREAD_STACK_SIZEOF(logger_stack),
        logging_thread,
        nullptr,
        nullptr,
        nullptr,
        6,            // Lower priority than main
        0,
        K_NO_WAIT
    );

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

    if (mode != Mode::Stopped) {
        k_mutex_unlock(&control_mutex);
        return -EALREADY;
    }

    sample_count = 0;
    storage_error_count = 0;
    last_storage_error = 0;

    k_sem_reset(&wake_signal);

    mode = Mode::Running;

    k_mutex_unlock(&control_mutex);

    k_sem_give(&start_signal);

    return 0;
}

int stop()
{
    k_mutex_lock(&control_mutex, K_FOREVER);

    if (mode == Mode::Stopped) {
        k_mutex_unlock(&control_mutex);
        return 0;
    }

    if (mode == Mode::Stopping) {
        k_mutex_unlock(&control_mutex);
        return -EBUSY;
    }

    mode = Mode::Stopping;

    k_mutex_unlock(&control_mutex);

    // Wake the sampling thread if it's sleeping.
    k_sem_give(&wake_signal);

    // Wait until the in-progress sample and
    // storage operation have completed.
    int ret = k_sem_take(
        &stopped_signal,
        K_SECONDS(10)
    );

    if (ret < 0) {
        return ret;
    }

    // A mounted SD card can now be flushed safely.
    auto status = storage::get_status();

    if (status.mounted) {
        return storage::flush_pending();
    }

    // Data remains in RAM when the card is absent.
    return 0;
}

bool active()
{
    k_mutex_lock(&control_mutex, K_FOREVER);

    bool result = (mode != Mode::Stopped);

    k_mutex_unlock(&control_mutex);

    return result;
}

Stats get_stats()
{
    k_mutex_lock(&control_mutex, K_FOREVER);

    Stats stats{};

    stats.running = (mode == Mode::Running);
    stats.stopping = (mode == Mode::Stopping);

    stats.samples = sample_count;
    stats.storage_errors = storage_error_count;
    stats.last_storage_error = last_storage_error;

    k_mutex_unlock(&control_mutex);

    return stats;
}

} // namespace sensor_logging