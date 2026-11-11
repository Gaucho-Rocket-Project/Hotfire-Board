#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include <errno.h>

#include "app/sensor_logging.hpp"
#include "drivers/sd.hpp"

namespace {

static int cmd_sys_reset(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "Preparing system reset...");

    if (sensor_logging::active()) {

        shell_print(sh, "Stopping sensor logging...");

        int ret = sensor_logging::stop();

        if (ret < 0) {
            shell_error(
                sh,
                "Reset aborted: logging stop failed (%d)",
                ret
            );

            return ret;
        }
    }

    auto sd = storage::get_status();

    if (sd.mounted) {

        // Record the reset before unmounting.
        (void)storage::log_status(
            storage::LogLevel::Info,
            "System reset requested"
        );

        shell_print(sh, "Unmounting SD card...");

        int ret = storage::unmount_sd();

        if (ret < 0) {
            shell_error(
                sh,
                "Reset aborted: SD unmount failed (%d)",
                ret
            );

            return ret;
        }

        shell_print(sh, "SD card safely unmounted");

    } else {

        if (sd.pending_events > 0 ||
            sd.pending_samples > 0) {

            shell_warn(
                sh,
                "SD not mounted; buffered RAM logs will be lost"
            );
        }
    }

    shell_print(sh, "Resetting STM32L496...");

    k_msleep(150);
    sys_reboot(SYS_REBOOT_COLD);
}

// Subcommands under "sys"
SHELL_STATIC_SUBCMD_SET_CREATE(
    sys_commands,

    SHELL_CMD(
        reset,
        NULL,
        "Reset the microcontroller",
        cmd_sys_reset
    ),

    SHELL_SUBCMD_SET_END
);

// Root command: sys
SHELL_CMD_REGISTER(
    sys,
    &sys_commands,
    "System management commands",
    NULL
);

} // namespace
