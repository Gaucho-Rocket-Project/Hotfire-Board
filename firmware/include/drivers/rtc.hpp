#ifndef DRIVERS_RTC_HPP
#define DRIVERS_RTC_HPP

namespace rtc_clock {

// RTC calendar values are interpreted as UTC. Never invent a date if unset.
struct Timestamp {
    char utc[21] = "UNSET";
};

Timestamp now();
// Strict YYYY-MM-DDTHH:MM:SSZ, returning 0 or a negative errno.
int set_utc(const char* timestamp);

} // namespace rtc_clock

#endif
