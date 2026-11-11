#include "drivers/sd.hpp"
#include "drivers/utils/buffer.hpp"

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
constexpr char SYSTEM_PATH[] = "/SD:/SYSTEM.LOG";
constexpr char SENSOR_PATH[] = "/SD:/SENSORS.CSV";

constexpr char CSV_HEADER[] =
    "uptime_ms,tc1_mc,tc2_mc,tc3_mc,tc4_mc,"
    "p1_raw,p2_raw,p3_raw,p4_raw\r\n";

constexpr size_t MAX_EVENTS = 64;
constexpr size_t MAX_SAMPLES = 128;
constexpr size_t EVENT_LENGTH = 144;

struct Event {
    char text[EVENT_LENGTH];
};

K_MUTEX_DEFINE(sd_mutex);

FATFS fat_fs{};
fs_mount_t sd_mount{};

bool mounted = false;

RingBuffer<Event, MAX_EVENTS> events;
RingBuffer<SensorSample, MAX_SAMPLES> samples;

// The following helpers are called with sd_mutex held.

int write_append(
    const char* path,
    const char* data,
    size_t length)
{
    fs_file_t file{};
    fs_file_t_init(&file);

    int ret = fs_open(
        &file,
        path,
        FS_O_CREATE | FS_O_WRITE | FS_O_APPEND
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
        "[%lld ms] %-5s %s\r\n",
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

int ensure_csv_header()
{
    fs_dirent entry{};

    int ret = fs_stat(SENSOR_PATH, &entry);

    if (ret == 0 && entry.size > 0) {
        return 0;
    }

    if (ret < 0 && ret != -ENOENT) {
        return ret;
    }

    return write_append(
        SENSOR_PATH,
        CSV_HEADER,
        sizeof(CSV_HEADER) - 1
    );
}

int flush_buffers()
{
    if (!mounted) {
        return -ENODEV;
    }

    // Flush system events first.
    while (const Event* event = events.front()) {

        int ret = write_append(
            SYSTEM_PATH,
            event->text,
            strlen(event->text)
        );

        if (ret < 0) {
            return ret;
        }

        events.pop();
    }

    // Flush sensor records to CSV.
    while (const SensorSample* sample = samples.front()) {

        char line[192];

        int length = snprintf(
            line,
            sizeof(line),
            "%lld,%ld,%ld,%ld,%ld,%u,%u,%u,%u\r\n",
            static_cast<long long>(sample->uptime_ms),
            static_cast<long>(sample->temperature_mc[0]),
            static_cast<long>(sample->temperature_mc[1]),
            static_cast<long>(sample->temperature_mc[2]),
            static_cast<long>(sample->temperature_mc[3]),
            static_cast<unsigned>(sample->pressure_raw[0]),
            static_cast<unsigned>(sample->pressure_raw[1]),
            static_cast<unsigned>(sample->pressure_raw[2]),
            static_cast<unsigned>(sample->pressure_raw[3])
        );

        if (length < 0) {
            return -EIO;
        }

        if (static_cast<size_t>(length) >= sizeof(line)) {
            return -ENOSPC;
        }

        int ret = write_append(
            SENSOR_PATH,
            line,
            static_cast<size_t>(length)
        );

        if (ret < 0) {
            return ret;
        }

        samples.pop();
    }

    return 0;
}

} // namespace

int mount_sd()
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    bool newly_mounted = !mounted;
    int ret = 0;

    if (newly_mounted) {
        sd_mount.type = FS_FATFS;
        sd_mount.mnt_point = MOUNT_POINT;
        sd_mount.fs_data = &fat_fs;
        sd_mount.flags =
            FS_MOUNT_FLAG_NO_FORMAT |
            FS_MOUNT_FLAG_USE_DISK_ACCESS;

        ret = fs_mount(&sd_mount);

        if (ret == 0) {
            mounted = true;
        }
    }

    if (ret == 0) {
        ret = ensure_csv_header();
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
        LOG_INF("SD card ready; pending logs flushed");
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
    k_mutex_lock(&sd_mutex, K_FOREVER);

    samples.push(sample);

    int ret = 0;

    if (mounted) {
        ret = flush_buffers();
    }

    k_mutex_unlock(&sd_mutex);
    return ret;
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
        ret = flush_buffers();
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

BufferStatus get_status()
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    BufferStatus result{
        mounted,
        events.size(),
        samples.size(),
        events.dropped_count(),
        samples.dropped_count()
    };

    k_mutex_unlock(&sd_mutex);
    return result;
}

} // namespace storage
