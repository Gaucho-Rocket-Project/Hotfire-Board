#include "app/rtc_uart.hpp"
#include "drivers/uart.hpp"
#include "drivers/rtc.hpp"
#include "drivers/sd.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void RtcUart::reply(const char* text)
{
    k_mutex_lock(&tx_mutex_, K_FOREVER);
    uart_.write(reinterpret_cast<const uint8_t*>(text), strlen(text));
    k_mutex_unlock(&tx_mutex_);
}

void RtcUart::poll()
{
    // Bound each pass so continuous input cannot starve the application FSM.
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t byte;
        int ret = uart_.read_byte(byte);
        if (ret == -EOVERFLOW) {
            parser_.invalidate();
            reply("RTC ERR RX_OVERFLOW\r\n");
            continue;
        }
        if (ret < 0) {
            break;
        }
        const auto command = parser_.feed(byte);
        if (command == RtcProtocol::Command::None) {
            continue;
        }
        if (command == RtcProtocol::Command::Invalid) {
            reply("RTC ERR INVALID_COMMAND\r\n");
            continue;
        }
        if (command == RtcProtocol::Command::Set) {
            ret = rtc_clock::set_utc(parser_.timestamp());
            char response[64];
            if (ret < 0) {
                snprintf(response, sizeof(response), "RTC ERR SET_FAILED %d\r\n", ret);
            } else {
                snprintf(response, sizeof(response), "RTC OK %s\r\n", parser_.timestamp());
            }
            reply(response);
            if (ret == 0) {
                (void)storage::log_status(storage::LogLevel::Info, "RTC set via USART1");
            }
        } else {
            const auto timestamp = rtc_clock::now();
            char response[64];
            snprintf(response, sizeof(response), "RTC TIME %s\r\n", timestamp.utc);
            reply(response);
        }
    }
}
