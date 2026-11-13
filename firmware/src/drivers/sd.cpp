#include "drivers/sd.hpp"
#include "drivers/rtc.hpp"
#include "drivers/utils/buffer.hpp"
#include "drivers/utils/log_session.hpp"
#include "drivers/utils/sample_csv.hpp"
#include <zephyr/sys/atomic.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/entropy.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <ff.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(sd_logger, LOG_LEVEL_INF);

namespace storage {
namespace {

constexpr char MOUNT_POINT[] = "/SD:";

constexpr size_t MAX_EVENTS = 64;
constexpr size_t MAX_SAMPLES = 512;
constexpr size_t BATCH_SAMPLES = 32;
constexpr size_t EVENT_LENGTH = 176;

struct Event {
    char text[EVENT_LENGTH];
};

K_MUTEX_DEFINE(sd_mutex);

FATFS fat_fs{};
fs_mount_t sd_mount{};

bool mounted = false;
bool logs_ready = false;
LogSession session;

RingBuffer<Event, MAX_EVENTS> events;
// Producers never acquire sd_mutex or wait for a filesystem operation.
K_MSGQ_DEFINE(sample_queue, sizeof(SensorSample), MAX_SAMPLES, 4);
atomic_t dropped_samples{};
SensorSample pending_samples[BATCH_SAMPLES];
size_t pending_count = 0;
char batch_data[BATCH_SAMPLES * CSV_LINE_SIZE];
off_t pending_offset = -1;
uint64_t written_samples = 0;
uint32_t write_errors = 0;
int last_write_error = 0;
uint32_t last_batch_us = 0;
uint32_t max_batch_us = 0;

// The following helpers are called with sd_mutex held.

int session_random(uint8_t* bytes, size_t length)
{
    const device* rng = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
    if (!device_is_ready(rng)) {
        return -ENODEV;
    }
    return entropy_get_entropy(rng, bytes, static_cast<uint16_t>(length));
}

int write_file(const char* path, const char* data, size_t length,
               fs_mode_t mode = FS_O_APPEND)
{
    fs_file_t file{};
    fs_file_t_init(&file);

    int ret = fs_open(
        &file,
        path,
        FS_O_CREATE | FS_O_WRITE | mode
    );

    if (ret < 0) {
        return ret;
    }

    ssize_t written = fs_write(&file, data, length);

    if (written < 0) {
        ret = static_cast<int>(written);
    } else if (static_cast<size_t>(written) != length) {
        ret = -EIO;
    } else {
        ret = fs_sync(&file);
    }

    int close_ret = fs_close(&file);

    if (ret == 0 && close_ret < 0) {
        ret = close_ret;
    }

    return ret;
}

int queue_event(LogLevel level, const char* message)
{
    Event event{};
    const auto timestamp = rtc_clock::now();

    const char* name = "INFO";

    switch (level) {
    case LogLevel::Info:
        name = "INFO";
        break;
    case LogLevel::Warning:
        name = "WARN";
        break;
    case LogLevel::Error:
        name = "ERROR";
        break;
    }

    int length = snprintf(
        event.text,
        sizeof(event.text),
        "[%s] [%lld ms] %-5s %s\r\n",
        timestamp.utc,
        static_cast<long long>(k_uptime_get()),
        name,
        message
    );

    if (length < 0) {
        return -EIO;
    }

    if (static_cast<size_t>(length) >= sizeof(event.text)) {
        return -ENOSPC;
    }

    events.push(event);
    return 0;
}

// sd_mutex held. Keep the starting offset until the entire batch is synced
// and closed. A retry truncates a partial/uncertain write before rewriting it.
int write_sample_batch(size_t length)
{
    fs_file_t file{};
    fs_file_t_init(&file);
    int ret = fs_open(&file, session.sensor_path, FS_O_CREATE | FS_O_WRITE);
    if (ret < 0) {
        return ret;
    }
    if (pending_offset < 0) {
        ret = fs_seek(&file, 0, FS_SEEK_END);
        if (ret == 0) {
            pending_offset = fs_tell(&file);
            if (pending_offset < 0) {
                ret = static_cast<int>(pending_offset);
            }
        }
    } else {
        ret = fs_truncate(&file, pending_offset);
        if (ret == 0) {
            ret = fs_seek(&file, pending_offset, FS_SEEK_SET);
        }
    }
    if (ret == 0) {
        const ssize_t written = fs_write(&file, batch_data, length);
        ret = written < 0 ? static_cast<int>(written) :
              static_cast<size_t>(written) != length ? -EIO : fs_sync(&file);
    }
    const int close_ret = fs_close(&file);
    return ret < 0 ? ret : close_ret;
}

int flush_buffers(bool drain_all = false)
{
    if (!mounted) {
        return -ENODEV;
    }

    if (!logs_ready) {
        // Reserve a fresh directory before writing anything. If setup fails,
        // keep the reservation and all queued records for the next attempt.
        int ret = session.create(session_random, fs_mkdir);
        if (ret == 0) {
            // No samples are written until setup completes. Rewrite the header
            // on retries so a partial initialization write cannot corrupt it.
            ret = write_file(session.sensor_path, CSV_HEADER,
                             sizeof(CSV_HEADER) - 1, FS_O_TRUNC);
        }
        if (ret == 0) {
            ret = write_file(session.system_path, "", 0);
        }
        if (ret < 0) {
            return ret;
        }
        logs_ready = true;
        LOG_INF("SD log session: %s", session.directory);
    }

    // Flush system events first.
    while (const Event* event = events.front()) {

        int ret = write_file(
            session.system_path,
            event->text,
            strlen(event->text)
        );

        if (ret < 0) {
            return ret;
        }

        events.pop();
    }

    do {
        // Retain a failed batch for retry before taking newer queued records.
        if (pending_count == 0) {
            while (pending_count < BATCH_SAMPLES &&
                   k_msgq_get(&sample_queue, &pending_samples[pending_count], K_NO_WAIT) == 0) {
                ++pending_count;
            }
        }
        if (pending_count == 0) {
            break;
        }
        const uint32_t start = k_cycle_get_32();
        size_t used = 0;
        int ret = 0;
        for (size_t i = 0; i < pending_count; ++i) {
            const int n = format_sensor_csv(pending_samples[i], batch_data + used,
                                            sizeof(batch_data) - used);
            if (n < 0) {
                ret = n;
                break;
            }
            used += static_cast<size_t>(n);
        }
        if (ret == 0) {
            ret = write_sample_batch(used);
        }
        last_batch_us = static_cast<uint32_t>(k_cyc_to_us_floor64(k_cycle_get_32() - start));
        if (last_batch_us > max_batch_us) {
            max_batch_us = last_batch_us;
        }
        if (ret < 0) {
            ++write_errors;
            last_write_error = ret;
            return ret;
        }
        written_samples += pending_count;
        pending_count = 0;
        pending_offset = -1;
    } while (drain_all);

    return 0;
}

void storage_thread(void*, void*, void*)
{
    while (true) {
        // Batch at most 32 records per pass, up to 100 passes/second.
        k_msleep(10);
        k_mutex_lock(&sd_mutex, K_FOREVER);
        if (mounted) {
            (void)flush_buffers();
        }
        k_mutex_unlock(&sd_mutex);
    }
}

K_THREAD_DEFINE(storage_writer, 3072, storage_thread, nullptr, nullptr, nullptr,
                7, 0, 0);

static const struct gpio_dt_spec sd_cd = GPIO_DT_SPEC_GET(DT_NODELABEL(sdmmc1), cd_gpios);

} // namespace


int mount_sd()
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    bool newly_mounted = !mounted;
    int ret = 0;


    if (newly_mounted) {

        int present = card_present();

        if (present <= 0) {
            k_mutex_unlock(&sd_mutex);

            if (present == 0) {
                return -ENODEV;
            }

            return present;
        }

        sd_mount.type = FS_FATFS;
        sd_mount.mnt_point = MOUNT_POINT;
        sd_mount.fs_data = &fat_fs;
        sd_mount.flags =
            FS_MOUNT_FLAG_NO_FORMAT |
            FS_MOUNT_FLAG_USE_DISK_ACCESS;

        ret = fs_mount(&sd_mount);

        if (ret == 0) {
            mounted = true;
            // Remounts also get a fresh session, including when cards are swapped.
            session = {};
            pending_offset = -1;
            logs_ready = false;
        }
    }

    if (ret == 0 && newly_mounted) {
        ret = queue_event(
            LogLevel::Info,
            "SD card mounted"
        );
    }

    if (ret == 0) {
        ret = flush_buffers();
    }

    if (ret < 0 && !mounted) {
        char message[64];

        snprintf(
            message,
            sizeof(message),
            "SD mount failed: %d",
            ret
        );

        (void)queue_event(LogLevel::Error, message);
    }

    k_mutex_unlock(&sd_mutex);

    if (ret == 0) {
        LOG_INF("SD card ready; queued logs draining");
    } else {
        LOG_ERR("SD mount or flush failed: %d", ret);
    }

    return ret;
}

int log_status(LogLevel level, const char* message)
{
    if (message == nullptr) {
        return -EINVAL;
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);

    int ret = queue_event(level, message);

    if (ret == 0 && mounted) {
        ret = flush_buffers();
    }

    k_mutex_unlock(&sd_mutex);
    return ret;
}

int append_startup_record()
{
    return log_status(
        LogLevel::Info,
        "DAQ firmware started"
    );
}

int append_sensor_sample(const SensorSample& sample)
{
    if (k_msgq_put(&sample_queue, &sample, K_NO_WAIT) != 0) {
        atomic_inc(&dropped_samples);
        return -ENOSPC;
    }
    return 0;
}

int unmount_sd()
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    if (!mounted) {
        k_mutex_unlock(&sd_mutex);
        return 0;
    }

    int ret = queue_event(
        LogLevel::Info,
        "SD eject requested"
    );

    if (ret == 0) {
        ret = flush_buffers(true);
    }

    if (ret == 0) {
        ret = fs_unmount(&sd_mount);

        if (ret == 0) {
            mounted = false;
        }
    }

    k_mutex_unlock(&sd_mutex);

    if (ret == 0) {
        LOG_INF("SD card safely unmounted");
    } else {
        LOG_ERR("SD eject failed: %d", ret);
    }

    return ret;
}


int card_present()
{
    if (!gpio_is_ready_dt(&sd_cd)) {
        return -ENODEV;
    }

    // PC7 has an external pull-up on the PCB.
    // Configure as input without another pull-up.
    int ret = gpio_pin_configure_dt(
        &sd_cd,
        GPIO_INPUT
    );

    if (ret < 0) {
        return ret;
    }

    // GPIO_ACTIVE_LOW:
    // 1 = inserted
    // 0 = absent
    return gpio_pin_get_dt(&sd_cd);
}


BufferStatus get_status()
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    BufferStatus result{
        mounted,
        events.size(),
        k_msgq_num_used_get(&sample_queue) + pending_count,
        events.dropped_count(),
        static_cast<uint32_t>(atomic_get(&dropped_samples)),
        {},
        written_samples,
        write_errors,
        last_write_error,
        last_batch_us,
        max_batch_us
    };
    snprintf(result.session_directory, sizeof(result.session_directory),
             "%s", session.directory);

    k_mutex_unlock(&sd_mutex);
    return result;
}

int flush_pending()
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    if (!mounted) {
        k_mutex_unlock(&sd_mutex);
        return -ENODEV;
    }

    int ret = flush_buffers(true);

    k_mutex_unlock(&sd_mutex);

    return ret;
}

} // namespace storage
