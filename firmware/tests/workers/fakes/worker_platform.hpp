#pragma once
// Run the actual acquisition lifecycle against host threads and timers. This
// tests synchronization, not Zephyr priorities, DMA, or real-time performance.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <cerrno>
using Clock = std::chrono::steady_clock;
using Micros = std::chrono::microseconds;
constexpr int K_FOREVER = -1, K_NO_WAIT = 0;
#define CONFIG_SYS_CLOCK_TICKS_PER_SEC 10000
#define K_USEC(n) (n)
#define K_SECONDS(n) ((n) * 1000000)
struct k_mutex { std::mutex value; };
#define K_MUTEX_DEFINE(name) k_mutex name
inline int k_mutex_lock(k_mutex* m, int) { m->value.lock(); return 0; }
inline int k_mutex_unlock(k_mutex* m) { m->value.unlock(); return 0; }
struct k_spinlock { std::mutex value; };
inline int k_spin_lock(k_spinlock* m) { m->value.lock(); return 0; }
inline void k_spin_unlock(k_spinlock* m, int) { m->value.unlock(); }
using atomic_t = std::atomic<int>;
inline int atomic_get(const atomic_t* a) { return a->load(); }
inline void atomic_set(atomic_t* a, int value) { a->store(value); }
struct k_sem {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count, limit;
    k_sem(unsigned count, unsigned limit) : count(count), limit(limit) {}
};
#define K_SEM_DEFINE(name, count, limit) k_sem name{count, limit}
inline int k_sem_take(k_sem* s, int timeout) {
    std::unique_lock lock(s->mutex);
    if (timeout == K_FOREVER) {
        s->changed.wait(lock, [&] { return s->count != 0; });
    } else if (!s->changed.wait_for(lock, Micros(timeout), [&] { return s->count != 0; })) {
        return -EAGAIN;
    }
    --s->count;
    return 0;
}
inline void k_sem_give(k_sem* s) {
    std::lock_guard lock(s->mutex);
    if (s->count < s->limit) { ++s->count; }
    s->changed.notify_one();
}
inline void k_sem_reset(k_sem* s) { std::lock_guard lock(s->mutex); s->count = 0; }
struct k_timer {
    std::mutex mutex;
    std::condition_variable changed;
    bool running = false;
    Clock::time_point next;
    Micros period{1};
};
#define K_TIMER_DEFINE(name, expiry, stop) k_timer name
inline void k_timer_start(k_timer* t, int delay, int period) {
    std::lock_guard lock(t->mutex);
    t->running = true;
    t->next = Clock::now() + Micros(delay);
    t->period = Micros(period);
    t->changed.notify_all();
}
inline void k_timer_stop(k_timer* t) {
    std::lock_guard lock(t->mutex);
    t->running = false;
    t->changed.notify_all();
}
inline uint32_t k_timer_status_sync(k_timer* t) {
    std::unique_lock lock(t->mutex);
    while (t->running && Clock::now() < t->next) { t->changed.wait_until(lock, t->next); }
    if (!t->running) { return 0; }
    const auto count = 1 + (Clock::now() - t->next) / t->period;
    t->next += count * t->period;
    return static_cast<uint32_t>(count);
}
inline uint64_t k_uptime_ticks() {
    return std::chrono::duration_cast<Micros>(Clock::now().time_since_epoch()).count();
}
inline uint64_t k_ticks_to_us_floor64(uint64_t t) { return t; }
inline uint32_t k_cycle_get_32() { return static_cast<uint32_t>(k_uptime_ticks()); }
inline uint64_t k_cyc_to_us_floor64(uint64_t c) { return c; }
struct k_thread {};
#define K_THREAD_STACK_DEFINE(name, size) char name[size]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)
inline void k_thread_create(k_thread*, char*, size_t, void (*fn)(void*, void*, void*),
                            void* a, void* b, void* c, int, int, int) {
    std::thread(fn, a, b, c).detach();
}
inline void k_thread_name_set(k_thread*, const char*) {}

class Thermocouples {
public:
    static constexpr size_t CHANNEL_COUNT = 4;
    struct Reading { int32_t temperature_mc = 25000; uint8_t fault = 0; bool valid = true; };
    bool channel_enabled(size_t ch) const { return ch < 4; }
    uint8_t enabled_mask() const { return 15; }
    int read_cold_junction(size_t, int32_t& value) { value = 25000; return 0; }
    int read(size_t, Reading& reading) { reading = {}; return 0; }
};
class PressureTransducers {
public:
    static constexpr size_t CHANNEL_COUNT = 4;
    struct Reading { uint16_t raw = 1; int32_t current_ua = 4000; };
    int read_all(Reading (&data)[4], uint8_t& valid) {
        for (auto& value : data) { value = {}; }
        valid = 15;
        return 0;
    }
};
class ADXL345 {
public:
    static constexpr unsigned RATE_HZ = 800, FIFO_DEPTH = 32;
    struct Reading { int16_t raw[3]{}; int32_t mg[3]{}; bool fifo_overrun = false; };
    bool available = true;
    bool fail_clear = false;
    std::atomic<unsigned> reads{0};
    std::atomic<bool> fail_read_once{false};
    std::mutex mutex;
    std::deque<Reading> fifo;
    bool ready() const { return available; }
    int clear_fifo() {
        std::lock_guard lock(mutex);
        if (fail_clear) { return -EIO; }
        fifo.clear(); return 0;
    }
    int read(Reading& reading) {
        ++reads;
        std::lock_guard lock(mutex);
        if (fail_read_once.exchange(false)) { return -EIO; }
        if (fifo.empty()) { return -EAGAIN; }
        reading = fifo.front(); fifo.pop_front(); return 0;
    }
};
namespace rtc_clock {
struct Timestamp { char utc[21] = "UNSET"; };
inline Timestamp now() { return {}; }
}
namespace storage {
enum class SampleType { Temperature, Pressure, Acceleration };
struct SensorSample {
    SampleType type = SampleType::Temperature;
    uint64_t uptime_us = 0;
    int64_t uptime_ms = 0;
    uint32_t acquisition_us = 0;
    char timestamp_utc[21] = "UNSET";
    int32_t cold_junction_mc[4]{}, temperature_mc[4]{}, pressure_ua[4]{};
    uint16_t pressure_raw[4]{};
    uint8_t tc_fault[4]{}, cj_valid_mask = 0, tc_valid_mask = 0;
    uint8_t fault_valid_mask = 0, pressure_valid_mask = 0;
    int16_t acceleration_raw[3]{};
    int32_t acceleration_mg[3]{};
    bool acceleration_valid = false, acceleration_fifo_overrun = false;
    uint32_t acceleration_run = 0, acceleration_segment = 0, acceleration_odr_hz = 0;
    uint64_t acceleration_sequence = 0, acceleration_nominal_us = 0;
};
class SDCard {
public:
    std::mutex mutex;
    std::vector<SensorSample> samples;
    unsigned flushes = 0;
    std::atomic<bool> drop_acceleration_once{false};
    static SDCard& instance() { static SDCard sd; return sd; }
    int enqueue(const SensorSample& sample) {
        if (sample.type == SampleType::Acceleration && drop_acceleration_once.exchange(false)) {
            return -ENOSPC;
        }
        std::lock_guard lock(mutex); samples.push_back(sample); return 0;
    }
    int flush() { std::lock_guard lock(mutex); ++flushes; return 0; }
};
}
