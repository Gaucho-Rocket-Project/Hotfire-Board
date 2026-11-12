
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdio.h>

#include "bsp.hpp"
#include "drivers/led.hpp"
#include "drivers/thermocouples.hpp"
#include "drivers/pressure.hpp"
#include "drivers/uart.hpp"
#include "drivers/sd.hpp"
#include "shell/daq.hpp"
#include "app/fsm.hpp"
#include "app/telemetry.hpp"
#include "app/rtc_uart.hpp"
#include "app/sensor_logging.hpp"

LOG_MODULE_REGISTER(daq, LOG_LEVEL_INF);

int main()
{
    LOG_INF("STM32L496 Data Logger Starting");

    (void)storage::append_startup_record();

    LED status_led(bsp::user_led);
    FSM fsm(status_led);
    UART uart(bsp::uart1);
    Thermocouples thermocouples;
    PressureTransducers pressure;

    int uart_ret = uart.init();

    if (uart_ret < 0) {
        LOG_ERR("USART1 initialization failed: %d", uart_ret);

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
    RtcUart rtc_uart(uart, tx_mutex);

    TelemetryTask telemetry(uart, tx_mutex);

    int telemetry_ret = telemetry.start();

    if (telemetry_ret < 0) {
        LOG_ERR("Telemetry task failed: %d", telemetry_ret);

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

    int thermo_ret = thermocouples.init();


    if (thermo_ret < 0) {
        LOG_ERR(
            "MAX31856 initialization failed: %d",
            thermo_ret
        );

        (void)storage::log_status(
            storage::LogLevel::Error,
            "MAX31856 initialization failed"
        );

    } else {

        LOG_INF("All four MAX31856 amplifiers ready");

        (void)storage::log_status(
            storage::LogLevel::Info,
            "MAX31856 amplifiers initialized"
        );
    }

    daq_shell_bind_thermocouples(thermocouples);

    int pressure_ret = pressure.init();
    if (pressure_ret < 0) {
        LOG_ERR("Pressure ADC initialization failed: %d", pressure_ret);
        (void)storage::log_status(storage::LogLevel::Error,
                                 "Pressure ADC initialization failed; affected channels will be blank");
    } else {
        LOG_INF("All four pressure ADC inputs initialized");
        (void)storage::log_status(storage::LogLevel::Info,
                                 "Pressure ADC inputs initialized (PC0-PC3, 130 ohm shunts)");
    }

    // A failed sensor group must not prevent the other group from logging.
    int logger_ret = sensor_logging::initialize(thermocouples, pressure);
    if (logger_ret < 0) {
        LOG_ERR("Sensor logger initialization failed: %d", logger_ret);
    } else {
        LOG_INF("Sensor logging task initialized");
    }

    fsm.start();

    LOG_INF("Application FSM started");

    (void)storage::log_status(
        storage::LogLevel::Info,
        "Application FSM started"
    );

    int present = storage::card_present();

    if (present == 1) {

        LOG_INF("SD card detected on PC7");

        int sd_ret = storage::mount_sd();

        if (sd_ret < 0) {
            LOG_ERR("SD mount failed: %d", sd_ret);

            (void)storage::log_status(
                storage::LogLevel::Error,
                "SD mount failed"
            );
        } else {
            LOG_INF("SD mounted; pending logs flushed");

            // Begin acquisition as soon as the startup SD session is ready.
            // start() also verifies that sensor logging initialized successfully.
            int start_ret = sensor_logging::start();

            if (start_ret < 0) {
                LOG_ERR("Automatic sensor logging start failed: %d", start_ret);

                (void)storage::log_status(
                    storage::LogLevel::Error,
                    "Automatic sensor logging start failed"
                );
            } else {
                LOG_INF("Sensor logging started automatically at 1 Hz");

                (void)storage::log_status(
                    storage::LogLevel::Info,
                    "Sensor logging started automatically at boot (1 Hz)"
                );
            }
        }

    } else if (present == 0) {

        LOG_WRN("No SD card inserted; automatic sensor logging skipped");

        (void)storage::log_status(
            storage::LogLevel::Info,
            "No SD card detected at startup"
        );

    } else {

        LOG_ERR("SD card-detect GPIO error: %d", present);

        (void)storage::log_status(
            storage::LogLevel::Error,
            "SD card detection failed"
        );
    }

    LOG_INF("DAQ application ready");

    (void)storage::log_status(
        storage::LogLevel::Info,
        "DAQ application ready"
    );

    while (true) {

        fsm.step();

        rtc_uart.poll();

        k_msleep(10);
    }

    return 0;
}
