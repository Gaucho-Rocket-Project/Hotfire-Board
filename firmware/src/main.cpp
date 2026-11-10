
#include <zephyr/kernel.h>

#include "bsp.hpp"
#include "drivers/led.hpp"
#include "drivers/uart.hpp"
#include "app/fsm.hpp"
#include "app/telemetry.hpp"

int main()
{
    LED status_led(bsp::user_led);
    FSM fsm(status_led);
    UART uart(bsp::uart1);

    if (uart.init() < 0) {
        return -1;
    }

    // Shared TX lock prevents messages from interleaving.
    k_mutex tx_mutex;
    k_mutex_init(&tx_mutex);

    TelemetryTask telemetry(uart, tx_mutex);

    if (telemetry.start() < 0) {
        return -1;
    }

    fsm.start();

    while (true) {
        fsm.step();

        uint8_t received;

        while (uart.read_byte(received) == 0) {
            k_mutex_lock(&tx_mutex, K_FOREVER);
            uart.write(&received, 1);
            k_mutex_unlock(&tx_mutex);
        }

        k_msleep(10);
    }

    return 0;
}
