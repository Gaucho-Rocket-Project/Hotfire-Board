#ifndef DRIVERS_SD_H
#define DRIVERS_SD_H

#include <cstddef>
#include <cstdint>
#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "drivers/utils/buffer.hpp"
#include "drivers/utils/log_session.hpp"

namespace storage {

enum class LogLevel { Info, Warning, Error };
enum class SampleType : uint8_t { Temperature, Pressure };

struct SensorSample {
    SampleType type = SampleType::Temperature;
    uint64_t uptime_us = 0;
    uint32_t acquisition_us = 0;
    int64_t uptime_ms = 0;
    char timestamp_utc[21] = "UNSET";
    int32_t cold_junction_mc[4]{};
    int32_t temperature_mc[4]{};
    uint8_t tc_fault[4]{};
    uint8_t cj_valid_mask = 0;
    uint8_t tc_valid_mask = 0;
    uint8_t fault_valid_mask = 0;
    uint16_t pressure_raw[4]{};
    int32_t pressure_ua[4]{};
    uint8_t pressure_valid_mask = 0;
};

struct BufferStatus {
    bool mounted = false;
    size_t pending_events = 0;
    size_t pending_samples = 0;
    uint32_t dropped_events = 0;
    uint32_t dropped_samples = 0;
    char session_directory[40]{};
    uint64_t written_samples = 0;
    uint32_t write_errors = 0;
    int last_write_error = 0;
    uint32_t last_batch_us = 0;
    uint32_t max_batch_us = 0;
};

/** Owns SD card lifecycle, filesystem handles, events, and asynchronous sensor logging.
 *  Call initialize() once from main before any other method.
 *  Sampling threads must only call enqueue(); no filesystem work is performed there.
 */
class SDCard final {
public:
    static SDCard& instance();

    SDCard(const SDCard&) = delete;
    SDCard& operator=(const SDCard&) = delete;

    int initialize();
    int card_present() const;  // 1=inserted, 0=absent, <0=error
    int mount();
    // Read a complete file while preventing eject through this SD owner.
    // capacity is the maximum byte count; no terminating NUL is added.
    // length is zero on failure. Oversized files return -EFBIG.
    int read_file(const char* path, char* buffer, size_t capacity, size_t& length);
    int eject();               // Call only after sensor acquisition has stopped.
    int flush();               // Drain queue and sync both files; may block.
    BufferStatus status();
    int log(LogLevel level, const char* message);
    int log_startup();
    int enqueue(const SensorSample& sample); // Nonblocking, -ENOSPC if full

private:
    SDCard() = default;

    static constexpr size_t kQueueSamples = 512;
    static constexpr size_t kBatchSamples = 32;
    static constexpr size_t kMaxEvents = 64;
    static constexpr size_t kEventLength = 176;
    static constexpr int64_t kSyncPeriodMs = 250;
    static constexpr unsigned kMaxBatchesPerPass = 8;
    static constexpr size_t kCsvLineSize = 384;

    struct Event { char text[kEventLength]{}; };

    static void thread_entry(void* arg, void*, void*);
    static int random_bytes(uint8_t* out, size_t size);
    void writer_loop();

    // All *_locked methods require mutex_ held by a non-acquisition thread.
    int open_session_locked();
    int drain_locked(bool all);
    int write_batch_locked();
    int write_event_locked();
    int sync_locked(bool force);
    void record_error_locked(int code);

    k_mutex mutex_{};
    k_sem wake_{};
    k_msgq sample_queue_{};
    k_thread writer_thread_{};
    alignas(4) char queue_bytes_[kQueueSamples * sizeof(SensorSample)]{};

    FATFS fatfs_{};
    fs_mount_t mount_desc_{};
    fs_file_t sensor_file_{};
    fs_file_t system_file_{};
    bool initialized_ = false;
    int card_detect_error_ = 0;
    bool mounted_ = false;
    bool logs_ready_ = false;
    bool sensor_open_ = false;
    bool system_open_ = false;
    bool dirty_sensor_ = false;
    bool dirty_system_ = false;
    int64_t last_sync_ms_ = 0;

    LogSession session_{};
    RingBuffer<Event, kMaxEvents> events_{};
    atomic_t dropped_samples_{};
    SensorSample pending_[kBatchSamples]{};
    size_t pending_count_ = 0;
    off_t pending_offset_ = -1;
    char batch_bytes_[kBatchSamples * kCsvLineSize]{};
    off_t event_offset_ = -1;

    uint64_t accepted_samples_ = 0;
    uint64_t synced_samples_ = 0;
    uint32_t write_errors_ = 0;
    int last_write_error_ = 0;
    uint32_t last_batch_us_ = 0;
    uint32_t max_batch_us_ = 0;
};

} // namespace storage

#endif // DRIVERS_SD_H