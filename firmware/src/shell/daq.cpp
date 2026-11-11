
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "shell/daq.hpp"
#include "drivers/thermocouples.hpp"
#include "app/sensor_logging.hpp"
#include "drivers/sd.hpp"

namespace {

K_MUTEX_DEFINE(binding_mutex);

Thermocouples* tc_driver = nullptr;

// Format milli-degrees Celsius without floating point.
void format_temperature(int32_t temperature_mc, char* buffer, size_t size)
{
    int64_t magnitude =
        temperature_mc < 0
            ? -static_cast<int64_t>(temperature_mc)
            : static_cast<int64_t>(temperature_mc);

    snprintf(
        buffer,
        size,
        "%s%lld.%03lld C",
        temperature_mc < 0 ? "-" : "",
        static_cast<long long>(magnitude / 1000),
        static_cast<long long>(magnitude % 1000)
    );
}

int cmd_daq_tc(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    k_mutex_lock(&binding_mutex, K_FOREVER);
    Thermocouples* driver = tc_driver;
    k_mutex_unlock(&binding_mutex);

    if (driver == nullptr) {
        shell_error(
            sh,
            "MAX31856 driver not initialized"
        );
        return -ENODEV;
    }

    shell_print(sh, "");
    shell_print(sh, "MAX31856 Thermocouple Measurements");
    shell_print(sh, "-----------------------------------------");

    int first_error = 0;

    for (size_t ch = 0;
         ch < Thermocouples::CHANNEL_COUNT;
         ++ch) {

        int32_t cj_temperature_mc = 0;

        int cj_ret = driver->read_cold_junction(
            ch,
            cj_temperature_mc
        );

        Thermocouples::Reading reading{};
        int tc_ret = driver->read(ch, reading);

        char internal_text[32]{};
        char external_text[64]{};

        // Internal cold-junction temperature
        if (cj_ret == 0) {
            format_temperature(
                cj_temperature_mc,
                internal_text,
                sizeof(internal_text)
            );
        } else {
            snprintf(
                internal_text,
                sizeof(internal_text),
                "READ ERROR (%d)",
                cj_ret
            );

            if (first_error == 0) {
                first_error = cj_ret;
            }
        }

        // External thermocouple temperature
        if (reading.valid) {
            format_temperature(
                reading.temperature_mc,
                external_text,
                sizeof(external_text)
            );
        }
        else if (reading.fault & 0x01U) {
            snprintf(
                external_text,
                sizeof(external_text),
                "NOT CONNECTED"
            );
        }
        else if (reading.fault != 0) {
            snprintf(
                external_text,
                sizeof(external_text),
                "INVALID (fault 0x%02X)",
                static_cast<unsigned>(reading.fault)
            );
        }
        else {
            snprintf(
                external_text,
                sizeof(external_text),
                "READ ERROR (%d)",
                tc_ret
            );

            if (first_error == 0 && tc_ret < 0) {
                first_error = tc_ret;
            }
        }

        shell_print(
            sh,
            "TC%u",
            static_cast<unsigned>(ch + 1)
        );

        shell_print(
            sh,
            "  Internal:     %s",
            internal_text
        );

        shell_print(
            sh,
            "  Thermocouple: %s",
            external_text
        );

        shell_print(
            sh,
            "  Fault:        0x%02X",
            static_cast<unsigned>(reading.fault)
        );

        shell_print(sh, "");
    }

    return first_error;
}


static int cmd_daq_log_start(
    const struct shell* sh,
    size_t argc,
    char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = sensor_logging::start();

    if (ret < 0) {
        shell_error(
            sh,
            "Cannot start logging: %d",
            ret
        );
        return ret;
    }

    (void)storage::log_status(
        storage::LogLevel::Info,
        "Sensor logging started"
    );

    auto sd = storage::get_status();

    shell_print(
        sh,
        "Sensor logging started at 1 Hz"
    );

    if (!sd.mounted) {
        shell_warn(
            sh,
            "SD not mounted: buffering in RAM only"
        );
    }

    return 0;
}

static int cmd_daq_log_stop(
    const struct shell* sh,
    size_t argc,
    char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = sensor_logging::stop();

    if (ret < 0) {
        shell_error(
            sh,
            "Stop or SD flush failed: %d",
            ret
        );
        return ret;
    }

    (void)storage::log_status(
        storage::LogLevel::Info,
        "Sensor logging stopped"
    );

    auto stats = sensor_logging::get_stats();
    auto sd = storage::get_status();

    shell_print(sh, "Sensor logging stopped");

    shell_print(
        sh,
        "Samples acquired: %u",
        static_cast<unsigned>(stats.samples)
    );

    if (!sd.mounted) {
        shell_warn(
            sh,
            "SD not mounted; samples remain in RAM"
        );
    } else if (sd.pending_samples > 0) {
        shell_warn(
            sh,
            "%u samples still pending",
            static_cast<unsigned>(sd.pending_samples)
        );
    } else {
        shell_print(sh, "Pending sensor records flushed");
    }

    return 0;
}

static int cmd_daq_log_status(
    const struct shell* sh,
    size_t argc,
    char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    auto stats = sensor_logging::get_stats();
    auto sd = storage::get_status();

    const char* state =
        stats.stopping ? "STOPPING" :
        stats.running ? "RUNNING" : "STOPPED";

    shell_print(sh, "");
    shell_print(sh, "DAQ Sensor Logging");
    shell_print(sh, "---------------------------");

    shell_print(sh, "State: %s", state);
    shell_print(sh, "Interval: 1000 ms");

    shell_print(
        sh,
        "Samples acquired: %u",
        static_cast<unsigned>(stats.samples)
    );

    shell_print(
        sh,
        "Storage errors: %u",
        static_cast<unsigned>(stats.storage_errors)
    );

    shell_print(
        sh,
        "Last storage error: %d",
        stats.last_storage_error
    );

    shell_print(
        sh,
        "SD: %s",
        sd.mounted ? "MOUNTED" : "UNMOUNTED"
    );

    shell_print(
        sh,
        "Pending samples: %u",
        static_cast<unsigned>(sd.pending_samples)
    );

    shell_print(
        sh,
        "Dropped samples: %u",
        static_cast<unsigned>(sd.dropped_samples)
    );

    shell_print(sh, "");

    return 0;
}


// Register "daq log start|stop|status".
SHELL_STATIC_SUBCMD_SET_CREATE(
    daq_log_commands,
    SHELL_CMD_ARG(
        start, NULL, "Start sensor logging at 1 Hz",
        cmd_daq_log_start, 1, 0
    ),
    SHELL_CMD_ARG(
        stop, NULL, "Stop sensor logging and flush pending records",
        cmd_daq_log_stop, 1, 0
    ),
    SHELL_CMD_ARG(
        status, NULL, "Show sensor logging and buffer status",
        cmd_daq_log_status, 1, 0
    ),
    SHELL_SUBCMD_SET_END
);

// Register "daq tc" and the logging subcommands.
SHELL_STATIC_SUBCMD_SET_CREATE(
    daq_commands,
    SHELL_CMD(
        log,
        &daq_log_commands,
        "Sensor logging controls",
        NULL
    ),
    SHELL_CMD(
        tc,
        NULL,
        "Read internal and external temperatures",
        cmd_daq_tc
    ),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(
    daq,
    &daq_commands,
    "DAQ diagnostic commands",
    NULL
);

} // anonymous namespace

void daq_shell_bind_thermocouples(
    Thermocouples& thermocouples)
{
    k_mutex_lock(&binding_mutex, K_FOREVER);
    tc_driver = &thermocouples;
    k_mutex_unlock(&binding_mutex);
}
