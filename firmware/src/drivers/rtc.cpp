#include "drivers/rtc.hpp"
#include "drivers/utils/utc_time.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/rtc.h>
#include <ff.h>
#include <time.h>

namespace {

bool read_time(rtc_time& time)
{
    const device* rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
    if (!device_is_ready(rtc) || rtc_get_time(rtc, &time) < 0) {
        return false;
    }

    // The STM32 calendar supports 2000 through 2099.
    return time.tm_year >= 100 && time.tm_year <= 199 &&
           time.tm_mon >= 0 && time.tm_mon <= 11 &&
           time.tm_mday >= 1 && time.tm_mday <= 31 &&
           time.tm_hour >= 0 && time.tm_hour <= 23 &&
           time.tm_min >= 0 && time.tm_min <= 59 &&
           time.tm_sec >= 0 && time.tm_sec <= 59;
}

} // namespace

rtc_clock::Timestamp rtc_clock::now()
{
    Timestamp stamp;
    rtc_time time{};
    if (read_time(time)) {
        strftime(stamp.utc, sizeof(stamp.utc), "%Y-%m-%dT%H:%M:%SZ",
                 rtc_time_to_tm(&time));
    }
    return stamp;
}

int rtc_clock::set_utc(const char* timestamp)
{
    tm parsed{};
    if (!parse_utc(timestamp, parsed)) {
        return -EINVAL;
    }
    const device* rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
    if (!device_is_ready(rtc)) {
        return -ENODEV;
    }
    rtc_time time{};
    time.tm_year = parsed.tm_year;
    time.tm_mon = parsed.tm_mon;
    time.tm_mday = parsed.tm_mday;
    time.tm_hour = parsed.tm_hour;
    time.tm_min = parsed.tm_min;
    time.tm_sec = parsed.tm_sec;
    time.tm_wday = parsed.tm_wday;
    time.tm_yday = parsed.tm_yday;
    time.tm_isdst = 0;
    time.tm_nsec = 0;
    return rtc_set_time(rtc, &time);
}

// FatFs file/directory modification times use the same RTC (two-second resolution).
extern "C" DWORD get_fattime(void)
{
    rtc_time time{};
    if (!read_time(time)) {
        return (1U << 21) | (1U << 16); // FAT epoch: 1980-01-01
    }
    return (static_cast<DWORD>(time.tm_year - 80) << 25) |
           (static_cast<DWORD>(time.tm_mon + 1) << 21) |
           (static_cast<DWORD>(time.tm_mday) << 16) |
           (static_cast<DWORD>(time.tm_hour) << 11) |
           (static_cast<DWORD>(time.tm_min) << 5) |
           static_cast<DWORD>(time.tm_sec / 2);
}
