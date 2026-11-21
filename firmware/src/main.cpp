#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <errno.h>

#include "app/config.hpp"

#include "bsp.hpp"
#include "drivers/led.hpp"
#include "drivers/thermocouples.hpp"
#include "drivers/pressure.hpp"
#include "drivers/adxl345.hpp"
#include "drivers/uart.hpp"
#include "drivers/sd.hpp"
#include "shell/daq.hpp"
#include "app/fsm.hpp"
#include "app/telemetry.hpp"
#include "app/rtc_uart.hpp"
#include "app/sensor_logging.hpp"

LOG_MODULE_REGISTER(daq, LOG_LEVEL_INF);

namespace {
// UART and its DMA contexts must persist for the lifetime of the firmware.
UART uart(bsp::uart1);

void startup_message(storage::LogLevel level, const char* message)
{
    switch (level) {
    case storage::LogLevel::Info: LOG_INF("%s", message); break;
    case storage::LogLevel::Warning: LOG_WRN("%s", message); break;
    case storage::LogLevel::Error: LOG_ERR("%s", message); break;
    }
    (void)storage::SDCard::instance().log(level, message);
}

int load_tc_config(storage::SDCard& sd, ThermocoupleConfig& config)
{
    // Boot-only buffer; do not consume half of the 4 KB main stack.
    static char text[THERMOCOUPLE_CONFIG_MAX_BYTES];
    size_t length = 0;
    int ret = sd.read_file("/SD:/config.ini", text, sizeof(text), length);
    if (ret == -ENOENT) {
        startup_message(storage::LogLevel::Warning,
                        "config.ini missing; using all four channels, Type K");
        config = ThermocoupleConfig{};
        return 0;
    }
    if (ret < 0) {
        char message[128];
        snprintf(message, sizeof(message),
                 "config.ini read failed (%d); thermocouple initialization skipped", ret);
        startup_message(storage::LogLevel::Error, message);
        return ret;
    }

    ConfigError error{};
    ret = parse_thermocouple_config(text, length, config, error);
    if (ret < 0) {
        char message[160];
        snprintf(message, sizeof(message), "config.ini line %u: %s",
                 static_cast<unsigned>(error.line), error.message);
        startup_message(storage::LogLevel::Error, message);
        return ret;
    }
    startup_message(storage::LogLevel::Info, "Thermocouple configuration loaded from config.ini");
    return 0;
}
}

int main()
{
    LOG_INF("STM32L496 Data Logger Starting");

    // Initialize the SD owner / RAM queue / worker before logging any events.
    auto& sd = storage::SDCard::instance();
    const int sd_init_ret = sd.initialize();
    if (sd_init_ret < 0) {
        LOG_ERR("SD logger initialization failed: %d", sd_init_ret);
        return sd_init_ret;
    }
    (void)sd.log_startup();

    LED status_led(bsp::user_led);
    FSM fsm(status_led);
    Thermocouples thermocouples;
    PressureTransducers pressure;
    ADXL345 accelerometer;

    const int uart_ret = uart.init();
    if (uart_ret < 0) {
        LOG_ERR("USART1 initialization failed: %d", uart_ret);
        (void)sd.log(storage::LogLevel::Error,
                     "USART1 initialization failed");
        return uart_ret;
    }

    LOG_INF("USART1 initialized");
    (void)sd.log(storage::LogLevel::Info, "USART1 initialized");

    k_mutex tx_mutex;
    k_mutex_init(&tx_mutex);
    RtcUart rtc_uart(uart, tx_mutex);
    TelemetryTask telemetry(uart, tx_mutex);

    const int telemetry_ret = telemetry.start();
    if (telemetry_ret < 0) {
        LOG_ERR("Telemetry task failed: %d", telemetry_ret);
        (void)sd.log(storage::LogLevel::Error,
                     "Telemetry task initialization failed");
        return telemetry_ret;
    }

    LOG_INF("Periodic telemetry started");
    (void)sd.log(storage::LogLevel::Info, "Periodic telemetry started");

    // Load configuration before touching thermocouple hardware.
    ThermocoupleConfig tc_config{};
    int config_ret = 0;
    bool sd_ready = false;
    const int present = sd.card_present();
    if (present == 1) {
        const int mount_ret = sd.mount();
        if (mount_ret == 0) {
            sd_ready = true;
            config_ret = load_tc_config(sd, tc_config);
        } else {
            config_ret = mount_ret;
            startup_message(storage::LogLevel::Error,
                            "SD mount failed; thermocouple initialization skipped");
        }
    } else if (present == 0) {
        startup_message(storage::LogLevel::Warning,
                        "No SD card; using all four channels, Type K; automatic logging skipped");
    } else {
        config_ret = present;
        startup_message(storage::LogLevel::Error,
                        "SD card detection failed; thermocouple initialization skipped");
    }

    if (config_ret == 0) {
        for (size_t ch = 0; ch < Thermocouples::CHANNEL_COUNT; ++ch) {
            char message[96];
            snprintf(message, sizeof(message), "TC%u requested: %s, type %s",
                     static_cast<unsigned>(ch + 1),
                     tc_config.channels[ch].enabled ? "enabled" : "disabled",
                     thermocouple_type_name(tc_config.channels[ch].type));
            startup_message(storage::LogLevel::Info, message);
        }
    }
    const int thermo_ret = config_ret == 0 ? thermocouples.init(tc_config) : config_ret;
    if (thermo_ret < 0) {
        const auto& startup = thermocouples.startup_status();
        char message[160];
        snprintf(message, sizeof(message), "MAX31856 initialization failed (%d): %s",
                 thermo_ret, startup.stage);
        startup_message(storage::LogLevel::Error, message);
        if (startup.channel < Thermocouples::CHANNEL_COUNT) {
            snprintf(message, sizeof(message), "Failed TC%u / decoder Y%u",
                     static_cast<unsigned>(startup.channel + 1),
                     static_cast<unsigned>(bsp::thermocouple_decoder_addresses[startup.channel]));
            startup_message(storage::LogLevel::Error, message);
        }
        if (startup.bus_stage) {
            startup_message(storage::LogLevel::Error, startup.bus_stage);
        }
        if (startup.cr0_valid && startup.cr1_valid) {
            snprintf(message, sizeof(message),
                     "MAX31856 readback: CR0 0x%02X (expected 0x%02X), CR1 0x%02X (expected 0x%02X)",
                     startup.cr0, startup.expected_cr0, startup.cr1, startup.expected_cr1);
            startup_message(storage::LogLevel::Error, message);
        }
    } else {
        LOG_INF("Enabled MAX31856 amplifiers ready (mask 0x%02X)",
                static_cast<unsigned>(thermocouples.enabled_mask()));
        (void)sd.log(storage::LogLevel::Info,
                     "Enabled MAX31856 amplifiers initialized");
    }
    daq_shell_bind_thermocouples(thermocouples);

    const int pressure_ret = pressure.init();
    if (pressure_ret < 0) {
        LOG_ERR("Pressure ADC initialization failed: %d", pressure_ret);
        (void)sd.log(storage::LogLevel::Error,
                     "Pressure ADC initialization failed; affected channels will be blank");
    } else {
        LOG_INF("All four pressure ADC inputs initialized");
        (void)sd.log(storage::LogLevel::Info,
                     "Pressure ADC inputs initialized (PC0-PC3, 130 ohm shunts)");
    }
    daq_shell_bind_pressure(pressure);

    const int accel_ret = accelerometer.init();
    if (accel_ret < 0) {
        LOG_ERR("ADXL345 initialization failed: %d", accel_ret);
        (void)sd.log(storage::LogLevel::Error,
                     "ADXL345 unavailable; acceleration logging skipped");
    } else {
        startup_message(storage::LogLevel::Info,
<<<<<<< HEAD
                        "ADXL345 ready on CS4: 1600 Hz, full resolution, +/-16 g");
=======
                        "ADXL345 ready on CS13/Y13: 100 Hz, full resolution, +/-16 g");
>>>>>>> e3e554f49ec0ca02b72441b60e861ce2e07ad21f
    }

    // A failed sensor group must not prevent the other group from logging.
    const int logger_ret = sensor_logging::initialize(thermocouples, pressure, accelerometer);
    if (logger_ret < 0) {
        LOG_ERR("Sensor logger initialization failed: %d", logger_ret);
    } else {
        LOG_INF("Sensor logging task initialized");
    }

    fsm.start();
    LOG_INF("Application FSM started");
    (void)sd.log(storage::LogLevel::Info, "Application FSM started");

    // The mount was performed above; start only after both drivers and the logger.
    // TC configuration failure does not block pressure recording.
    if (sd_ready) {
        const int start_ret = sensor_logging::start();
        if (start_ret < 0) {
            LOG_ERR("Automatic sensor logging start failed: %d", start_ret);
            (void)sd.log(storage::LogLevel::Error,
                         "Automatic sensor logging start failed");
        } else {
            startup_message(storage::LogLevel::Info,
                            "Sensor logging started at boot (temperature 10 Hz, pressure 1000 Hz; ADXL345 1600 Hz if available)");
        }
    }

    LOG_INF("DAQ application ready");
    (void)sd.log(storage::LogLevel::Info, "DAQ application ready");

    while (true) {
        fsm.step();
        rtc_uart.poll();
        k_msleep(10);
    }

    return 0;
}