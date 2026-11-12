#ifndef APP_RTC_PROTOCOL_HPP
#define APP_RTC_PROTOCOL_HPP

#include "drivers/utils/utc_time.hpp"
#include <stddef.h>

class RtcProtocol {
public:
    enum class Command { None, Get, Set, Invalid };

    // Call after a UART receive error. Discard through the next newline,
    // so a fragment can never be interpreted as a complete command.
    void invalidate() { discard_ = true; }

    Command feed(unsigned char byte)
    {
        if (byte != '\n') {
            if (discard_) {
                return Command::None;
            }
            if (used_ == sizeof(line_) - 1 ||
                (byte != '\r' && (byte < 32 || byte > 126))) {
                discard_ = true;
            } else {
                line_[used_++] = static_cast<char>(byte);
            }
            return Command::None;
        }

        if (used_ && line_[used_ - 1] == '\r') {
            --used_;
        }
        line_[used_] = '\0';
        const bool invalid = discard_;
        used_ = 0;
        discard_ = false;
        if (invalid) {
            return Command::Invalid;
        }
        if (!line_[0]) {
            return Command::None;
        }
        if (strcmp(line_, "RTC GET") == 0) {
            return Command::Get;
        }
        tm parsed{};
        if (strncmp(line_, "RTC SET ", 8) == 0 &&
            rtc_clock::parse_utc(line_ + 8, parsed)) {
            return Command::Set;
        }
        return Command::Invalid;
    }

    // Valid until the next feed(), only after Command::Set.
    const char* timestamp() const { return line_ + 8; }

private:
    char line_[48]{};
    size_t used_ = 0;
    bool discard_ = false;
};

#endif
