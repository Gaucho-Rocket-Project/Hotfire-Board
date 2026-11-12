#include "drivers/sd.hpp"
#include "drivers/rtc.hpp"
#include "drivers/utils/buffer.hpp"
#include "drivers/utils/log_session.hpp"

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

constexpr char CSV_HEADER[] =
    "timestamp_utc,uptime_ms,"
    "tc1_cj_mc,tc1_mc,tc1_fault,"
    "tc2_cj_mc,tc2_mc,tc2_fault,"
    "tc3_cj_mc,tc3_mc,tc3_fault,"
    "tc4_cj_mc,tc4_mc,tc4_fault,"
    "p1_raw,p2_raw,p3_raw,p4_raw,"
    "p1_ua,p2_ua,p3_ua,p4_ua\r\n";

constexpr size_t MAX_EVENTS = 64;
constexpr size_t MAX_SAMPLES = 128;
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
RingBuffer<SensorSample, MAX_SAMPLES> samples;

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

int flush_buffers()
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

    while (!samples.empty()) {

        const SensorSample& sample = *samples.front();

        char cj[4][20]{};
        char tc[4][20]{};
        char fault[4][10]{};
        char pressure[4][12]{};

        // Format valid measurements only.
        for (size_t i = 0; i < 4; ++i) {
            uint8_t bit =
                static_cast<uint8_t>(1U << i);

            if (sample.cj_valid_mask & bit) {
                snprintf(
                    cj[i], sizeof(cj[i]),
                    "%ld",
                    static_cast<long>(
                        sample.cold_junction_mc[i]
                    )
                );
            }

            if (sample.tc_valid_mask & bit) {
                snprintf(
                    tc[i], sizeof(tc[i]),
                    "%ld",
                    static_cast<long>(
                        sample.temperature_mc[i]
                    )
                );
            }

            if (sample.fault_valid_mask & bit) {
                snprintf(
                    fault[i], sizeof(fault[i]),
                    "0x%02X",
                    static_cast<unsigned>(
                        sample.tc_fault[i]
                    )
                );
            }

            if (sample.pressure_valid_mask & bit) {
                snprintf(
                    pressure[i], sizeof(pressure[i]),
                    "%u",
                    static_cast<unsigned>(
                        sample.pressure_raw[i]
                    )
                );
            }
        }

        char line[384]{};
        size_t used = 0;

        int n = snprintf(
            line,
            sizeof(line),
            "%s,%lld",
            sample.timestamp_utc,
            static_cast<long long>(sample.uptime_ms)
        );

        if (n < 0) {
            return -EIO;
        }

        if (static_cast<size_t>(n) >= sizeof(line)) {
            return -ENOSPC;
        }

        used = static_cast<size_t>(n);

        // Write internal, external, and fault fields
        // for each of the four channels.
        for (size_t i = 0; i < 4; ++i) {
            n = snprintf(
                line + used,
                sizeof(line) - used,
                ",%s,%s,%s",
                cj[i],
                tc[i],
                fault[i]
            );

            if (n < 0) {
                return -EIO;
            }

            if (static_cast<size_t>(n) >=
                sizeof(line) - used) {
                return -ENOSPC;
            }

            used += static_cast<size_t>(n);
        }

        // Preserve the existing raw pressure column positions.
        for (size_t i = 0; i < 4; ++i) {
            n = snprintf(
                line + used,
                sizeof(line) - used,
                ",%s",
                pressure[i]
            );

            if (n < 0) {
                return -EIO;
            }

            if (static_cast<size_t>(n) >=
                sizeof(line) - used) {
                return -ENOSPC;
            }

            used += static_cast<size_t>(n);
        }

        for (size_t i = 0; i < 4; ++i) {
            if (sample.pressure_valid_mask & (1U << i)) {
                n = snprintf(line + used, sizeof(line) - used, ",%ld",
                             static_cast<long>(sample.pressure_ua[i]));
            } else {
                n = snprintf(line + used, sizeof(line) - used, ",");
            }
            if (n < 0) {
                return -EIO;
            }
            if (static_cast<size_t>(n) >= sizeof(line) - used) {
                return -ENOSPC;
            }
            used += static_cast<size_t>(n);
        }

        n = snprintf(
            line + used,
            sizeof(line) - used,
            "\r\n"
        );

        if (n < 0) {
            return -EIO;
        }

        if (static_cast<size_t>(n) >=
            sizeof(line) - used) {
            return -ENOSPC;
        }

        used += static_cast<size_t>(n);

        int ret = write_file(
            session.sensor_path,
            line,
            used
        );

        if (ret < 0) {
            // Keep the sample buffered for retry.
            return ret;
        }

        // Remove only after a successful write.
        samples.pop();
    }

    return 0;
}

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
        samples.size(),
        events.dropped_count(),
        samples.dropped_count(),
        {}
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

    int ret = flush_buffers();

    k_mutex_unlock(&sd_mutex);

    return ret;
}

} // namespace storage
