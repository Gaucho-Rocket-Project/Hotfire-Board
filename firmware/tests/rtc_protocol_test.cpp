// From firmware/: c++ -std=c++17 -Wall -Wextra -Werror -Iinclude
// tests/rtc_protocol_test.cpp -o /tmp/rtc_protocol_test && /tmp/rtc_protocol_test
#include "app/rtc_protocol.hpp"

#include <cassert>
#include <stdio.h>
#include <string>

using Command = RtcProtocol::Command;

Command feed(RtcProtocol& parser, const std::string& text)
{
    Command result = Command::None;
    for (unsigned char c : text) {
        const auto next = parser.feed(c);
        if (next != Command::None) {
            assert(result == Command::None);
            result = next;
        }
    }
    return result;
}

int main()
{
    // Compare every possible date in the supported range to the host's UTC
    // calendar, including leap days, weekdays, and year boundaries.
    for (int year = 2000; year <= 2099; ++year) {
        for (int month = 1; month <= 12; ++month) {
            for (int day = 1; day <= 31; ++day) {
                tm expected{};
                expected.tm_year = year - 1900;
                expected.tm_mon = month - 1;
                expected.tm_mday = day;
                expected.tm_hour = 23;
                expected.tm_min = 59;
                expected.tm_sec = 59;
                const time_t epoch = timegm(&expected);
                assert(epoch != -1);
                const bool valid = expected.tm_mon == month - 1 && expected.tm_mday == day;
                char text[32];
                snprintf(text, sizeof(text), "%04d-%02d-%02dT23:59:59Z", year, month, day);
                tm parsed{};
                assert(rtc_clock::parse_utc(text, parsed) == valid);
                if (valid) {
                    assert(parsed.tm_wday == expected.tm_wday);
                    assert(parsed.tm_yday == expected.tm_yday);
                    assert(timegm(&parsed) == epoch);
                }
            }
        }
    }

    for (const char* bad : {"", "1999-12-31T23:59:59Z", "2100-01-01T00:00:00Z",
                           "2026-00-01T00:00:00Z", "2026-13-01T00:00:00Z",
                           "2026-01-00T00:00:00Z", "2026-01-01T24:00:00Z",
                           "2026-01-01T00:60:00Z", "2026-01-01T00:00:60Z",
                           "2026-01-01T00:00:00", "2026-01-01T00:00:00Zx",
                           "2026-01-01 00:00:00Z", "2026-01-01T00:00:00z",
                           "2026-01-01T00:00:00+00:00", "202a-01-01T00:00:00Z"}) {
        tm parsed{};
        assert(!rtc_clock::parse_utc(bad, parsed));
    }

    RtcProtocol parser;
    assert(feed(parser, "RTC SET 2026-11-") == Command::None);
    assert(feed(parser, "11T18:30:45Z\r\n") == Command::Set);
    assert(std::string(parser.timestamp()) == "2026-11-11T18:30:45Z");
    assert(feed(parser, "RTC GET\n") == Command::Get);
    assert(feed(parser, "RTC GET\r\n") == Command::Get);
    assert(feed(parser, "\n") == Command::None);
    assert(feed(parser, "RTC SET 2025-02-29T00:00:00Z\n") == Command::Invalid);
    assert(feed(parser, "RTC SET 2024-02-29T00:00:00Z\n") == Command::Set);
    assert(feed(parser, "RTC SET 2026-11-11T18:30:45Z extra\n") == Command::Invalid);
    assert(feed(parser, "rtc get\n") == Command::Invalid);
    assert(feed(parser, "RTC\r GET\n") == Command::Invalid);
    assert(feed(parser, std::string("RTC GET\0ignored\n", 16)) == Command::Invalid);
    assert(feed(parser, std::string(100, 'x') + "RTC SET 2026-11-11T18:30:45Z\n") == Command::Invalid);
    assert(feed(parser, "RTC GET\n") == Command::Get);
    assert(feed(parser, "RTC SET 2026-") == Command::None);
    parser.invalidate();
    assert(feed(parser, "11-11T18:30:45Z\n") == Command::Invalid);
    assert(feed(parser, "RTC SET 2026-11-11T18:30:45Z\n") == Command::Set);
}
