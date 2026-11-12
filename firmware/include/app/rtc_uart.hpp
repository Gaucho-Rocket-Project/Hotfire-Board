#ifndef APP_RTC_UART_HPP
#define APP_RTC_UART_HPP

#include "app/rtc_protocol.hpp"
#include <zephyr/kernel.h>

class UART;

class RtcUart {
public:
    RtcUart(UART& uart, k_mutex& tx_mutex) : uart_(uart), tx_mutex_(tx_mutex) {}
    void poll();

private:
    UART& uart_;
    k_mutex& tx_mutex_;
    RtcProtocol parser_;
    void reply(const char* text);
};

#endif
