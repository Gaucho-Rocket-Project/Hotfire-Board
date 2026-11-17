#include "drivers/sd.hpp"
#include "app/sensor_logging.hpp"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <errno.h>

namespace {

storage::SDCard& card() { return storage::SDCard::instance(); }

int cmd_sd_mount(const shell* sh, size_t, char**)
{
    const int ret = card().mount();
    if (ret < 0) {
        shell_error(sh, "SD mount failed: %d", ret);
    } else {
        shell_print(sh, "SD mounted; session ready");
    }
    return ret;
}

int cmd_sd_eject(const shell* sh, size_t, char**)
{
    if (sensor_logging::active()) {
        shell_error(sh, "Stop sensor logging before SD eject");
        return -EBUSY;
    }
    const int ret = card().eject();
    if (ret < 0) {
        shell_error(sh, "SD eject failed: %d", ret);
    } else {
        shell_print(sh, "SD safely unmounted");
    }
    return ret;
}

int cmd_sd_sync(const shell* sh, size_t, char**)
{
    if (sensor_logging::active()) {
        shell_error(sh, "Stop sensor logging before a full flush/sync");
        return -EBUSY;
    }
    const int ret = card().flush();
    if (ret < 0) {
        shell_error(sh, "SD flush/sync failed: %d", ret);
    } else {
        shell_print(sh, "SD queue drained and files synced");
    }
    return ret;
}

int cmd_sd_status(const shell* sh, size_t, char**)
{
    const int presence = card().card_present();
    const auto status = card().status();
    shell_print(sh, "Card detected: %s",
                presence == 1 ? "YES" : presence == 0 ? "NO" : "GPIO ERROR");
    shell_print(sh, "SD: %s", status.mounted ? "MOUNTED" : "UNMOUNTED");
    shell_print(sh, "Session: %s", status.session_directory[0]
                ? status.session_directory : "not created");
    shell_print(sh, "Pending: events=%u, samples=%u",
                static_cast<unsigned>(status.pending_events),
                static_cast<unsigned>(status.pending_samples));
    shell_print(sh, "Dropped: events=%u, samples=%u",
                status.dropped_events, status.dropped_samples);
    shell_print(sh, "Samples synced: %llu",
                static_cast<unsigned long long>(status.written_samples));
    shell_print(sh, "Write errors: %u (last %d)",
                status.write_errors, status.last_write_error);
    shell_print(sh, "Batch us: last=%u, max=%u",
                status.last_batch_us, status.max_batch_us);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sd_commands,
    SHELL_CMD(mount, NULL, "Mount SD and prepare logging session", cmd_sd_mount),
    SHELL_CMD(eject, NULL, "Flush and safely unmount SD", cmd_sd_eject),
    SHELL_CMD(sync, NULL, "Drain queued logs and sync to SD", cmd_sd_sync),
    SHELL_CMD(status, NULL, "Display SD card and logger status", cmd_sd_status),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sd, &sd_commands, "SD card management", NULL);

} // namespace
