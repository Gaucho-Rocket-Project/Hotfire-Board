#ifndef DRIVERS_UTILS_UTC_TIME_HPP
#define DRIVERS_UTILS_UTC_TIME_HPP

#include <string.h>
#include <time.h>

namespace rtc_clock {

// Strict UTC format; STM32's calendar stores years 2000..2099.
inline bool parse_utc(const char* text, tm& result)
{
    if (!text || strlen(text) != 20) {
        return false;
    }
    constexpr char pattern[] = "dddd-dd-ddTdd:dd:ddZ";
    for (unsigned i = 0; i < 20; ++i) {
        if (pattern[i] == 'd') {
            if (text[i] < '0' || text[i] > '9') {
                return false;
            }
        } else if (text[i] != pattern[i]) {
            return false;
        }
    }
    auto pair = [text](unsigned i) { return (text[i] - '0') * 10 + text[i + 1] - '0'; };
    const int year = pair(0) * 100 + pair(2);
    const int month = pair(5);
    const int day = pair(8);
    const int hour = pair(11), minute = pair(14), second = pair(17);
    if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0; // All supported centuries are leap years.
    if (day < 1 || day > days[month - 1] + (month == 2 && leap)) {
        return false;
    }
    tm parsed{};
    parsed.tm_year = year - 1900;
    parsed.tm_mon = month - 1;
    parsed.tm_mday = day;
    parsed.tm_hour = hour;
    parsed.tm_min = minute;
    parsed.tm_sec = second;
    parsed.tm_yday = day - 1;
    for (int m = 1; m < month; ++m) {
        parsed.tm_yday += days[m - 1] + (m == 2 && leap);
    }
    int elapsed = parsed.tm_yday;
    for (int y = 2000; y < year; ++y) {
        elapsed += 365 + (y % 4 == 0);
    }
    parsed.tm_wday = (6 + elapsed) % 7; // 2000-01-01 was Saturday.
    result = parsed;
    return true;
}

} // namespace rtc_clock
#endif
