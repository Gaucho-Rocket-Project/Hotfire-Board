
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "bsp.hpp"
#include "drivers/led.hpp"
#include "drivers/uart.hpp"
#include "drivers/sd.hpp"
#include "app/fsm.hpp"
#include "app/telemetry.hpp"

LOG_MODULE_REGISTER(daq, LOG_LEVEL_INF);

int main()
{
    LOG_INF("STM32L496 Data Logger Starting");

    (void)storage::append_startup_record();

    LED status_led(bsp::user_led);
    FSM fsm(status_led);
    UART uart(bsp::uart1);

    int uart_ret = uart.init();

    if (uart_ret < 0) {
        LOG_ERR(
            "USART1 initialization failed: %d",
            uart_ret
        );

        (void)storage::log_status(
            storage::LogLevel::Error,
            "USART1 initialization failed"
        );

        return uart_ret;
    }

    LOG_INF("USART1 initialized");

    (void)storage::log_status(
        storage::LogLevel::Info,
        "USART1 initialized"
    );

    k_mutex tx_mutex;
    k_mutex_init(&tx_mutex);

    TelemetryTask telemetry(uart, tx_mutex);

    int telemetry_ret = telemetry.start();

    if (telemetry_ret < 0) {
        LOG_ERR(
            "Telemetry task failed: %d",
            telemetry_ret
        );

        (void)storage::log_status(
            storage::LogLevel::Error,
            "Telemetry task initialization failed"
        );

        return telemetry_ret;
    }

    LOG_INF("Periodic telemetry started");

    (void)storage::log_status(
        storage::LogLevel::Info,
        "Periodic telemetry started"
    );

    fsm.start();

    LOG_INF("Application FSM started");

    (void)storage::log_status(
        storage::LogLevel::Info,
        "Application FSM started"
    );

    int sd_ret = storage::mount_sd();

    if (sd_ret < 0) {
        LOG_WRN(
            "SD unavailable (%d); logs buffered in RAM",
            sd_ret
        );
    } else {
        LOG_INF("SD mounted; pending logs flushed");
    }

    LOG_INF("DAQ application ready");

    (void)storage::log_status(
        storage::LogLevel::Info,
        "DAQ application ready"
    );


    while (true) {
        fsm.step();

        uint8_t received;

        while (uart.read_byte(received) == 0) {
            k_mutex_lock(&tx_mutex,K_FOREVER);
            uart.write(&received, 1);
            k_mutex_unlock(&tx_mutex);
        }

        k_msleep(10);
    }

    return 0;
}
