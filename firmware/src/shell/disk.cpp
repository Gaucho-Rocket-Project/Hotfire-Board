#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <errno.h>

#include "drivers/sd.hpp"
#include "app/sensor_logging.hpp"


static int cmd_sd_mount(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int present = storage::card_present();

    if (present == 0) {
        shell_warn(sh, "No SD card inserted");
        return -ENODEV;
    }

    if (present < 0) {
        shell_error(
            sh,
            "Card detection failed: %d",
            present
        );

        return present;
    }

    shell_print(sh, "SD card detected, mounting...");

    int ret = storage::mount_sd();

    if (ret < 0) {
        shell_error(sh, "SD mount failed: %d", ret);
        return ret;
    }

    shell_print(
        sh,
        "SD mounted; queued logs draining"
    );

    return 0;
}


static int cmd_sd_eject(
    const struct shell* sh,
    size_t argc,
    char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (sensor_logging::active()) {
        shell_error(
            sh,
            "Stop sensor logging before ejecting SD"
        );

        return -EBUSY;
    }

    int ret = storage::unmount_sd();

    if (ret == 0) {
        shell_print(sh, "SD safely unmounted");
    } else {
        shell_error(sh, "SD eject failed: %d", ret);
    }

    return ret;
}

static int cmd_sd_status(
    const struct shell* sh,
    size_t argc,
    char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    auto status = storage::get_status();

    int present = storage::card_present();

    shell_print(
        sh,
        "Card detected: %s",
        present == 1 ? "YES" :
        present == 0 ? "NO" : "GPIO ERROR"
    );

    shell_print(
        sh,
        "SD: %s",
        status.mounted ? "MOUNTED" : "UNMOUNTED"
    );

    shell_print(sh, "Log session: %s",
                status.session_directory[0] ? status.session_directory : "not created");

    shell_print(
        sh,
        "Pending system events: %u",
        static_cast<unsigned>(status.pending_events)
    );

    shell_print(
        sh,
        "Pending sensor samples: %u",
        static_cast<unsigned>(status.pending_samples)
    );

    shell_print(
        sh,
        "Dropped system events: %u",
        static_cast<unsigned>(status.dropped_events)
    );

    shell_print(
        sh,
        "Dropped sensor samples: %u",
        static_cast<unsigned>(status.dropped_samples)
    );

    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sd_commands,
    SHELL_CMD(mount, NULL, "Mount SD and flush logs",
              cmd_sd_mount),
    SHELL_CMD(eject, NULL, "Safely unmount SD",
              cmd_sd_eject),
    SHELL_CMD(status, NULL, "Show SD buffer status",
              cmd_sd_status),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(
    sd,
    &sd_commands,
    "SD card management",
    NULL
);
