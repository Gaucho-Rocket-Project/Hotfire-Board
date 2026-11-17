#include "drivers/sd.hpp"

#include "drivers/rtc.hpp"
#include "drivers/utils/sample_csv.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(sd_logger, LOG_LEVEL_INF);

namespace storage {
namespace {
constexpr char kMountPoint[] = "/SD:";
K_THREAD_STACK_DEFINE(sd_writer_stack, 3072);
const gpio_dt_spec card_detect = GPIO_DT_SPEC_GET(DT_NODELABEL(sdmmc1), cd_gpios);
} // namespace

SDCard& SDCard::instance()
{
    static SDCard card;
    return card;
}

int SDCard::initialize()
{
    // Initialization belongs to main() and must complete before shell/sensor calls.
    if (initialized_) {
        return 0;
    }
    if (!gpio_is_ready_dt(&card_detect)) {
        card_detect_error_ = -ENODEV;
    } else {
        card_detect_error_ = gpio_pin_configure_dt(&card_detect, GPIO_INPUT);
    }

    k_mutex_init(&mutex_);
    k_sem_init(&wake_, 0, 1);
    k_msgq_init(&sample_queue_, queue_bytes_, sizeof(SensorSample), kQueueSamples);
    atomic_set(&dropped_samples_, 0);

    mount_desc_.type = FS_FATFS;
    mount_desc_.mnt_point = kMountPoint;
    mount_desc_.fs_data = &fatfs_;
    mount_desc_.flags = FS_MOUNT_FLAG_NO_FORMAT | FS_MOUNT_FLAG_USE_DISK_ACCESS;
    initialized_ = true;

    k_thread_create(&writer_thread_, sd_writer_stack,
                    K_THREAD_STACK_SIZEOF(sd_writer_stack),
                    thread_entry, this, nullptr, nullptr,
                    7, 0, K_NO_WAIT);
    k_thread_name_set(&writer_thread_, "sd_writer");
    return 0;
}

int SDCard::card_present() const
{
    if (!initialized_) {
        return -EAGAIN;
    }
    if (card_detect_error_ < 0) {
        return card_detect_error_;
    }
    // The STM32 SDMMC driver disconnects CD during disk deinitialization
    // (including fs_unmount). Restore input mode for status/remount checks.
    const int ret = gpio_pin_configure_dt(&card_detect, GPIO_INPUT);
    if (ret < 0) {
        return ret;
    }
    return gpio_pin_get_dt(&card_detect);
}

int SDCard::random_bytes(uint8_t* out, size_t size)
{
    const device* rng = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
    if (!device_is_ready(rng)) {
        return -ENODEV;
    }
    return entropy_get_entropy(rng, out, static_cast<uint16_t>(size));
}

void SDCard::record_error_locked(int code)
{
    if (code < 0) {
        ++write_errors_;
        last_write_error_ = code;
    }
}

int SDCard::open_session_locked()
{
    if (!mounted_) {
        return -ENODEV;
    }
    if (logs_ready_) {
        // Never reinitialize/truncate an existing session after a close error.
        return (sensor_open_ && system_open_) ? 0 : -EIO;
    }

    int ret = session_.create(random_bytes, fs_mkdir);
    if (ret < 0) {
        return ret;
    }

    // A fresh session is reserved before either file is initialized.
    // Until logs_ready_, no sensor records have been written to this file,
    // so retrying FS_O_TRUNC is safe after a failed setup attempt.
    fs_file_t_init(&sensor_file_);
    ret = fs_open(&sensor_file_, session_.sensor_path,
                  FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        return ret;
    }
    sensor_open_ = true;

    constexpr size_t header_length = sizeof(CSV_HEADER) - 1;
    ssize_t written = fs_write(&sensor_file_, CSV_HEADER, header_length);
    ret = written < 0 ? static_cast<int>(written) :
          static_cast<size_t>(written) != header_length ? -EIO : fs_sync(&sensor_file_);
    if (ret < 0) {
        (void)fs_close(&sensor_file_);
        sensor_open_ = false;
        return ret;
    }

    fs_file_t_init(&system_file_);
    ret = fs_open(&system_file_, session_.system_path,
                  FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
    if (ret < 0) {
        (void)fs_close(&sensor_file_);
        sensor_open_ = false;
        return ret;
    }
    system_open_ = true;
    ret = fs_seek(&system_file_, 0, FS_SEEK_END);
    if (ret < 0) {
        (void)fs_close(&system_file_);
        (void)fs_close(&sensor_file_);
        system_open_ = false;
        sensor_open_ = false;
        return ret;
    }
    logs_ready_ = true;
    last_sync_ms_ = k_uptime_get();
    LOG_INF("SD log session: %s", session_.directory);
    return 0;
}

int SDCard::mount()
{
    if (!initialized_) {
        return -EAGAIN;
    }
    k_mutex_lock(&mutex_, K_FOREVER);

    int ret = 0;
    if (!mounted_) {
        const int present = card_present();
        if (present <= 0) {
            ret = present == 0 ? -ENODEV : present;
        } else {
            ret = fs_mount(&mount_desc_);
            if (ret == 0) {
                mounted_ = true;
                logs_ready_ = false;
                session_ = {};
                pending_offset_ = -1;
                event_offset_ = -1;
                Event event{};
                snprintf(event.text, sizeof(event.text),
                              "[%lld ms] INFO SD card mounted\r\n",
                              static_cast<long long>(k_uptime_get()));
                events_.push(event);
            }
        }
    }
    if (ret == 0) {
        ret = open_session_locked();
    }
    if (ret == 0) {
        // Mount should return promptly, even if logging is already active.
        // The background writer drains any remaining RAM backlog.
        ret = drain_locked(false);
    }
    if (ret == 0) {
        ret = sync_locked(true);
    }
    if (ret < 0) {
        record_error_locked(ret);
    }
    k_mutex_unlock(&mutex_);
    k_sem_give(&wake_);
    return ret;
}

int SDCard::enqueue(const SensorSample& sample)
{
    if (!initialized_) {
        return -EAGAIN;
    }
    if (k_msgq_put(&sample_queue_, &sample, K_NO_WAIT) != 0) {
        atomic_inc(&dropped_samples_);
        return -ENOSPC;
    }
    k_sem_give(&wake_);
    return 0;
}

int SDCard::log(LogLevel level, const char* message)
{
    if (!initialized_) {
        return -EAGAIN;
    }
    if (message == nullptr) {
        return -EINVAL;
    }
    const auto timestamp = rtc_clock::now();
    const char* tag = level == LogLevel::Error ? "ERROR" :
                      level == LogLevel::Warning ? "WARN" : "INFO";
    Event event{};
    int length = snprintf(event.text, sizeof(event.text),
                               "[%s] [%lld ms] %-5s %s\r\n",
                               timestamp.utc,
                               static_cast<long long>(k_uptime_get()),
                               tag, message);
    if (length < 0) {
        return -EIO;
    }
    if (static_cast<size_t>(length) >= sizeof(event.text)) {
        return -ENOSPC;
    }
    k_mutex_lock(&mutex_, K_FOREVER);
    events_.push(event);
    k_mutex_unlock(&mutex_);
    k_sem_give(&wake_);
    return 0;
}

int SDCard::log_startup()
{
    return log(LogLevel::Info, "DAQ firmware started");
}

int SDCard::write_batch_locked()
{
    if (pending_count_ == 0) {
        while (pending_count_ < kBatchSamples &&
               k_msgq_get(&sample_queue_, &pending_[pending_count_], K_NO_WAIT) == 0) {
            ++pending_count_;
        }
    }
    if (pending_count_ == 0) {
        return 0;
    }
    const uint32_t start = k_cycle_get_32();
    size_t used = 0;
    for (size_t i = 0; i < pending_count_; ++i) {
        const int n = format_sensor_csv(pending_[i], batch_bytes_ + used,
                                        sizeof(batch_bytes_) - used);
        if (n < 0) {
            return n;
        }
        used += static_cast<size_t>(n);
    }
    int ret = 0;
    if (pending_offset_ < 0) {
        pending_offset_ = fs_tell(&sensor_file_);
        if (pending_offset_ < 0) {
            return static_cast<int>(pending_offset_);
        }
    } else {
        // A prior write may have been partial; restore file to the checkpoint.
        ret = fs_truncate(&sensor_file_, pending_offset_);
        if (ret == 0) {
            ret = fs_seek(&sensor_file_, pending_offset_, FS_SEEK_SET);
        }
    }
    if (ret == 0) {
        const ssize_t written = fs_write(&sensor_file_, batch_bytes_, used);
        ret = written < 0 ? static_cast<int>(written) :
              static_cast<size_t>(written) != used ? -EIO : 0;
    }

    last_batch_us_ = static_cast<uint32_t>(
        k_cyc_to_us_floor64(k_cycle_get_32() - start));
    if (last_batch_us_ > max_batch_us_) {
        max_batch_us_ = last_batch_us_;
    }
    if (ret < 0) {
        return ret; // Keep pending samples and pending_offset_ for retry.
    }
    accepted_samples_ += pending_count_;
    pending_count_ = 0;
    pending_offset_ = -1;
    dirty_sensor_ = true;
    return 0;
}

int SDCard::write_event_locked()
{
    const Event* event = events_.front();
    if (!event) {
        return 0;
    }
    int ret = 0;
    if (event_offset_ < 0) {
        event_offset_ = fs_tell(&system_file_);
        if (event_offset_ < 0) {
            return static_cast<int>(event_offset_);
        }
    } else {
        ret = fs_truncate(&system_file_, event_offset_);
        if (ret == 0) {
            ret = fs_seek(&system_file_, event_offset_, FS_SEEK_SET);
        }
    }
    if (ret == 0) {
        const size_t size = strlen(event->text);
        const ssize_t written = fs_write(&system_file_, event->text, size);
        ret = written < 0 ? static_cast<int>(written) :
              static_cast<size_t>(written) != size ? -EIO : 0;
    }
    if (ret < 0) {
        return ret;
    }
    events_.pop();
    event_offset_ = -1;
    dirty_system_ = true;
    return 0;
}

int SDCard::sync_locked(bool force)
{
    if (!logs_ready_) {
        return -ENODEV;
    }
    const int64_t now = k_uptime_get();
    if (!force && now - last_sync_ms_ < kSyncPeriodMs) {
        return 0;
    }
    if (dirty_sensor_ || force) {
        const int ret = fs_sync(&sensor_file_);
        if (ret < 0) {
            return ret;
        }
        dirty_sensor_ = false;
        synced_samples_ = accepted_samples_;
    }
    if (dirty_system_ || force) {
        const int ret = fs_sync(&system_file_);
        if (ret < 0) {
            return ret;
        }
        dirty_system_ = false;
    }
    last_sync_ms_ = now;
    return 0;
}

int SDCard::drain_locked(bool all)
{
    int ret = open_session_locked();
    if (ret < 0) {
        return ret;
    }

    // Prioritize sensor throughput. Cap a normal pass to make shell operations
    // responsive even when logging continuously.
    const unsigned limit = all ? 0xFFFFFFFFu : kMaxBatchesPerPass;
    unsigned count = 0;
    while ((pending_count_ || k_msgq_num_used_get(&sample_queue_) > 0) && count < limit) {
        ret = write_batch_locked();
        if (ret < 0) {
            return ret;
        }
        ++count;
    }
    // Drain events too. They never block the 1 kHz producer directly.
    while (events_.front()) {
        ret = write_event_locked();
        if (ret < 0) {
            return ret;
        }
        if (!all) {
            break;
        }
    }
    return sync_locked(false);
}

int SDCard::flush()
{
    if (!initialized_) {
        return -EAGAIN;
    }
    k_mutex_lock(&mutex_, K_FOREVER);
    const int ret = mounted_ ? drain_locked(true) : -ENODEV;
    const int sync_ret = ret == 0 ? sync_locked(true) : 0;
    const int result = ret < 0 ? ret : sync_ret;
    record_error_locked(result);
    k_mutex_unlock(&mutex_);
    return result;
}

int SDCard::eject()
{
    if (!initialized_) {
        return -EAGAIN;
    }
    k_mutex_lock(&mutex_, K_FOREVER);
    if (!mounted_) {
        k_mutex_unlock(&mutex_);
        return 0;
    }

    // Record the operator request before draining and syncing.
    Event eject_event{};
    snprintf(eject_event.text, sizeof(eject_event.text),
                  "[%lld ms] INFO SD eject requested\r\n",
                  static_cast<long long>(k_uptime_get()));
    events_.push(eject_event);

    int ret = drain_locked(true);
    if (ret == 0) {
        ret = sync_locked(true);
    }
    if (ret == 0 && sensor_open_) {
        ret = fs_close(&sensor_file_);
        sensor_open_ = false;
    }
    if (ret == 0 && system_open_) {
        ret = fs_close(&system_file_);
        system_open_ = false;
    }
    if (ret == 0) {
        ret = fs_unmount(&mount_desc_);
        if (ret == 0) {
            mounted_ = false;
            logs_ready_ = false;
            session_ = {};
            dirty_sensor_ = false;
            dirty_system_ = false;
        }
    }
    record_error_locked(ret);
    k_mutex_unlock(&mutex_);
    return ret;
}

BufferStatus SDCard::status()
{
    BufferStatus result{};
    if (!initialized_) {
        return result;
    }
    k_mutex_lock(&mutex_, K_FOREVER);
    result.mounted = mounted_;
    result.pending_events = events_.size();
    result.pending_samples = k_msgq_num_used_get(&sample_queue_) + pending_count_;
    result.dropped_events = events_.dropped_count();
    result.dropped_samples = static_cast<uint32_t>(atomic_get(&dropped_samples_));
    snprintf(result.session_directory, sizeof(result.session_directory),
                  "%s", session_.directory);
    result.written_samples = synced_samples_;
    result.write_errors = write_errors_;
    result.last_write_error = last_write_error_;
    result.last_batch_us = last_batch_us_;
    result.max_batch_us = max_batch_us_;
    k_mutex_unlock(&mutex_);
    return result;
}

void SDCard::thread_entry(void* arg, void*, void*)
{
    static_cast<SDCard*>(arg)->writer_loop();
}

void SDCard::writer_loop()
{
    while (true) {
        (void)k_sem_take(&wake_, K_MSEC(20));
        k_mutex_lock(&mutex_, K_FOREVER);
        int ret = 0;
        if (mounted_) {
            ret = drain_locked(false);
            record_error_locked(ret);
            if (ret == 0 && (pending_count_ || k_msgq_num_used_get(&sample_queue_) > 0)) {
                // Queue still contains samples after the capped burst.
                k_sem_give(&wake_);
            }
        }
        k_mutex_unlock(&mutex_);
        if (ret < 0) {
            k_msleep(100); // Avoid a busy loop after card or filesystem errors.
        }
    }
}

} // namespace storage
