#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "shell/daq.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include <errno.h>
#include <string.h>

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


/*
 * Expansion GPIO interface
 *
 * GPIO 0: PB0 -> J15 pin 6
 * GPIO 1: PC5 -> J15 pin 7
 *
 * Only these two pins are exposed for general I/O.
 */

struct ExpansionGpio {
    const struct device* port;
    gpio_pin_t pin;
    const char* name;
    unsigned int header_pin;
    bool output;
};

static ExpansionGpio gpio_lines[] = {
    {
        DEVICE_DT_GET(DT_NODELABEL(gpiob)),
        0,
        "PB0",
        6,
        false
    },
    {
        DEVICE_DT_GET(DT_NODELABEL(gpioc)),
        5,
        "PC5",
        7,
        false
    }
};

K_MUTEX_DEFINE(expansion_gpio_mutex);

static bool expansion_gpio_initialized = false;

static int gpio_index(const char* name)
{
    if (strcmp(name, "0") == 0 ||
        strcmp(name, "PB0") == 0) {
        return 0;
    }

    if (strcmp(name, "1") == 0 ||
        strcmp(name, "PC5") == 0) {
        return 1;
    }

    return -EINVAL;
}

// Called with expansion_gpio_mutex held.
static int initialize_expansion_gpio()
{
    if (expansion_gpio_initialized) {
        return 0;
    }

    // Verify the GPIO controllers are ready.
    for (auto& line : gpio_lines) {
        if (!device_is_ready(line.port)) {
            return -ENODEV;
        }
    }

    // Default to inputs to avoid unexpectedly
    // driving external devices at startup.
    for (auto& line : gpio_lines) {
        int ret = gpio_pin_configure(
            line.port,
            line.pin,
            GPIO_INPUT
        );

        if (ret < 0) {
            return ret;
        }

        line.output = false;
    }

    expansion_gpio_initialized = true;
    return 0;
}

// daq gpio list
static int cmd_gpio_list(
    const struct shell* sh,
    size_t argc,
    char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    k_mutex_lock(
        &expansion_gpio_mutex,
        K_FOREVER
    );

    int ret = initialize_expansion_gpio();

    if (ret < 0) {
        k_mutex_unlock(&expansion_gpio_mutex);
        shell_error(sh, "GPIO init failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Expansion GPIO (J15)");
    shell_print(sh, "---------------------------");

    for (unsigned int i = 0; i < 2; ++i) {
        auto& line = gpio_lines[i];

        int value = gpio_pin_get_raw(
            line.port,
            line.pin
        );

        if (value < 0) {
            shell_print(
                sh,
                "GPIO%u  %s  J15-%u  READ ERROR %d",
                i,
                line.name,
                line.header_pin,
                value
            );
            continue;
        }

        shell_print(
            sh,
            "GPIO%u  %s  J15-%u  %-6s %s",
            i,
            line.name,
            line.header_pin,
            line.output ? "OUTPUT" : "INPUT",
            value ? "HIGH" : "LOW"
        );
    }

    k_mutex_unlock(&expansion_gpio_mutex);

    shell_print(sh, "");
    return 0;
}

// daq gpio read <id>
static int cmd_gpio_read(
    const struct shell* sh,
    size_t argc,
    char** argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: daq gpio read <0|1>");
        return -EINVAL;
    }

    int index = gpio_index(argv[1]);

    if (index < 0) {
        shell_error(sh, "Unknown GPIO: %s", argv[1]);
        return -EINVAL;
    }

    k_mutex_lock(
        &expansion_gpio_mutex,
        K_FOREVER
    );

    int ret = initialize_expansion_gpio();

    if (ret == 0) {
        ret = gpio_pin_get_raw(
            gpio_lines[index].port,
            gpio_lines[index].pin
        );
    }

    k_mutex_unlock(&expansion_gpio_mutex);

    if (ret < 0) {
        shell_error(sh, "GPIO read failed: %d", ret);
        return ret;
    }

    shell_print(
        sh,
        "%s = %s (%d)",
        gpio_lines[index].name,
        ret ? "HIGH" : "LOW",
        ret
    );

    return 0;
}

// daq gpio write <id> <0|1>
static int cmd_gpio_write(
    const struct shell* sh,
    size_t argc,
    char** argv)
{
    if (argc != 3) {
        shell_error(
            sh,
            "Usage: daq gpio write <0|1> <0|1>"
        );
        return -EINVAL;
    }

    int index = gpio_index(argv[1]);

    if (index < 0) {
        shell_error(sh, "Unknown GPIO: %s", argv[1]);
        return -EINVAL;
    }

    int value;

    if (strcmp(argv[2], "0") == 0) {
        value = 0;
    } else if (strcmp(argv[2], "1") == 0) {
        value = 1;
    } else {
        shell_error(sh, "Value must be 0 or 1");
        return -EINVAL;
    }

    k_mutex_lock(
        &expansion_gpio_mutex,
        K_FOREVER
    );

    int ret = initialize_expansion_gpio();

    if (ret == 0) {
        auto& line = gpio_lines[index];

        if (!line.output) {
            // Configure as output with the requested
            // initial level to avoid a LOW glitch.
            ret = gpio_pin_configure(
                line.port,
                line.pin,
                value ? GPIO_OUTPUT_HIGH
                      : GPIO_OUTPUT_LOW
            );

            if (ret == 0) {
                line.output = true;
            }
        } else {
            ret = gpio_pin_set_raw(
                line.port,
                line.pin,
                value
            );
        }
    }

    k_mutex_unlock(&expansion_gpio_mutex);

    if (ret < 0) {
        shell_error(sh, "GPIO write failed: %d", ret);
        return ret;
    }

    shell_print(
        sh,
        "%s set %s",
        gpio_lines[index].name,
        value ? "HIGH" : "LOW"
    );

    return 0;
}

// daq gpio input <id>
static int cmd_gpio_input(
    const struct shell* sh,
    size_t argc,
    char** argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: daq gpio input <0|1>");
        return -EINVAL;
    }

    int index = gpio_index(argv[1]);

    if (index < 0) {
        shell_error(sh, "Unknown GPIO: %s", argv[1]);
        return -EINVAL;
    }

    k_mutex_lock(
        &expansion_gpio_mutex,
        K_FOREVER
    );

    int ret = initialize_expansion_gpio();

    if (ret == 0) {
        auto& line = gpio_lines[index];

        ret = gpio_pin_configure(
            line.port,
            line.pin,
            GPIO_INPUT
        );

        if (ret == 0) {
            line.output = false;
        }
    }

    k_mutex_unlock(&expansion_gpio_mutex);

    if (ret < 0) {
        shell_error(sh, "GPIO input failed: %d", ret);
        return ret;
    }

    shell_print(
        sh,
        "%s configured as INPUT",
        gpio_lines[index].name
    );

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

// GPIO subcommands.
SHELL_STATIC_SUBCMD_SET_CREATE(
    daq_gpio_commands,
    SHELL_CMD(
        list,
        NULL,
        "List expansion GPIO pins",
        cmd_gpio_list
    ),
    SHELL_CMD_ARG(
        read,
        NULL,
        "Read GPIO: read <0|1>",
        cmd_gpio_read,
        2,
        0
    ),
    SHELL_CMD_ARG(
        write,
        NULL,
        "Write GPIO: write <0|1> <0|1>",
        cmd_gpio_write,
        3,
        0
    ),
    SHELL_CMD_ARG(
        input,
        NULL,
        "Set GPIO to input: input <0|1>",
        cmd_gpio_input,
        2,
        0
    ),
    SHELL_SUBCMD_SET_END
);

// Register thermocouple, logging, and GPIO commands under one DAQ root.
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
    SHELL_CMD(
        gpio,
        &daq_gpio_commands,
        "Expansion GPIO read/write",
        NULL
    ),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(
    daq,
    &daq_commands,
    "DAQ diagnostics and controls",
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
