#include "app/sensor_logging.hpp"
#include "worker_platform.hpp"
#include <cassert>
#include <cstdlib>

template<class Predicate> void wait_for(Predicate predicate)
{
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        assert(Clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main()
{
    Thermocouples tc;
    PressureTransducers pressure;
    ADXL345 accel;
    auto& sd = storage::SDCard::instance();
    assert(sensor_logging::initialize(tc, pressure, accel) == 0);
    assert(sensor_logging::initialize(tc, pressure, accel) == -EALREADY);
    assert(sensor_logging::start() == 0);
    assert(sensor_logging::start() == -EALREADY);
    {
        std::lock_guard lock(accel.mutex);
        for (int16_t i = 0; i < 3; ++i) {
            ADXL345::Reading reading{};
            reading.raw[0] = i;
            reading.mg[0] = i * 4;
            reading.fifo_overrun = i == 0;
            accel.fifo.push_back(reading);
        }
    }
    wait_for([] { return sensor_logging::get_stats().accelerometer.samples == 3; });
    auto stats = sensor_logging::get_stats();
    assert(stats.accelerometer_fifo_overruns == 1);
    assert(stats.acceleration_sample_valid && stats.acceleration_raw[0] == 2);
    assert(sensor_logging::stop() == 0 && !sensor_logging::active());
    const auto stopped = sensor_logging::get_stats();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    assert(sensor_logging::get_stats().pressure.samples == stopped.pressure.samples);
    assert(sensor_logging::get_stats().accelerometer.samples == 3);
    {
        std::lock_guard lock(sd.mutex);
        unsigned count = 0;
        for (const auto& sample : sd.samples) {
            if (sample.type != storage::SampleType::Acceleration) { continue; }
            assert(sample.acceleration_valid && sample.acceleration_raw[0] == int(count++));
            assert(sample.pressure_valid_mask == 0 && sample.tc_valid_mask == 0);
        }
        assert(count == 3 && sd.flushes == 1);
    }
    // Restart clears old FIFO entries and resets cached samples/statistics.
    accel.fifo.push_back({});
    assert(sensor_logging::start() == 0);
    wait_for([] { return sensor_logging::get_stats().accelerometer_empty_polls > 0; });
    assert(sensor_logging::get_stats().accelerometer.samples == 0);
    assert(!sensor_logging::get_stats().acceleration_sample_valid);
    assert(sensor_logging::stop() == 0);

    // Missing sensor and failed FIFO setup must not add an unacknowledgeable worker.
    for (unsigned mode = 0; mode < 2; ++mode) {
        accel.available = mode != 0;
        accel.fail_clear = mode != 0;
        const auto reads = accel.reads.load();
        assert(sensor_logging::start() == 0);
        wait_for([] { return sensor_logging::get_stats().pressure.samples >= 3; });
        assert(!sensor_logging::get_stats().accelerometer_running);
        assert(sensor_logging::stop() == 0);
        assert(accel.reads.load() == reads);
    }
    assert(sensor_logging::get_stats().accelerometer.last_error == -EIO);
    // Exercise immediate stop before worker threads reach their timer wait.
    accel.available = true;
    accel.fail_clear = false;
    for (unsigned i = 0; i < 10; ++i) {
        assert(sensor_logging::start() == 0);
        assert(sensor_logging::stop() == 0);
    }
    std::_Exit(0); // Firmware workers intentionally run forever; skip global teardown.
}
