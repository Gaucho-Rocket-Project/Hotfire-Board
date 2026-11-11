#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <zephyr/version.h>

#include <errno.h>
#include <stdint.h>

#include "app/sensor_logging.hpp"
#include "drivers/sd.hpp"

namespace {

constexpr size_t RAM_BYTES = DT_REG_SIZE(DT_CHOSEN(zephyr_sram));
constexpr size_t FLASH_BYTES = DT_REG_SIZE(DT_CHOSEN(zephyr_flash));

static int cmd_sys_info(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "Firmware: DAQ data logger");
    shell_print(sh, "MCU family: %s", CONFIG_SOC);
    shell_print(sh, "Build target: %s", CONFIG_BOARD_TARGET);
    shell_print(sh, "Zephyr: %s", KERNEL_VERSION_STRING);
#ifdef BUILD_VERSION
    shell_print(sh, "Zephyr build: %s", STRINGIFY(BUILD_VERSION));
#endif
    shell_print(sh, "Configured flash: %zu KiB", FLASH_BYTES / 1024U);
    shell_print(sh, "Configured RAM: %zu KiB", RAM_BYTES / 1024U);
    shell_print(sh, "Uptime: %lld ms", static_cast<long long>(k_uptime_get()));

    uint8_t id[16]{};
    ssize_t length = hwinfo_get_device_id(id, sizeof(id));

    if (length > 0) {
        char hex_id[sizeof(id) * 2 + 1]{};
        bin2hex(id, static_cast<size_t>(length), hex_id, sizeof(hex_id));
        shell_print(sh, "Device ID: %s", hex_id);
    } else {
        shell_warn(sh, "Device ID unavailable (%d)", static_cast<int>(length));
    }

    return 0;
}

static void print_thread_stack(const struct k_thread* thread, void* user_data)
{
    const auto* sh = static_cast<const struct shell*>(user_data);
    const char* name = k_thread_name_get(const_cast<struct k_thread*>(thread));
    if (name == nullptr || name[0] == '\0') {
        name = "(unnamed)";
    }

    const size_t total = thread->stack_info.size;
    size_t unused = 0;
    int ret = k_thread_stack_space_get(thread, &unused);

    if (ret < 0 || total == 0 || unused > total) {
        shell_print(sh, "%p %-16s stack usage unavailable (%d)",
                    static_cast<const void*>(thread), name, ret < 0 ? ret : -EINVAL);
        return;
    }

    const size_t used = total - unused;
    const unsigned int percent =
        static_cast<unsigned int>((static_cast<uint64_t>(used) * 100U) / total);

    shell_print(sh, "%p %-16s %6zu %6zu %6zu %3u%%",
                static_cast<const void*>(thread), name, total, used, unused, percent);
}

static int cmd_sys_memory(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    // Linker symbols cover data, BSS, and noinit, including reserved stacks.
    const size_t reserved = reinterpret_cast<uintptr_t>(_image_ram_end) -
                            reinterpret_cast<uintptr_t>(_image_ram_start);

    shell_print(sh, "Configured RAM:       %zu bytes", RAM_BYTES);
    shell_print(sh, "Firmware RAM reserved: %zu bytes", reserved);
    if (reserved <= RAM_BYTES) {
        shell_print(sh, "Unreserved RAM:       %zu bytes", RAM_BYTES - reserved);
    }
    shell_print(sh, "System heap reserved: %u bytes",
                static_cast<unsigned int>(CONFIG_HEAP_MEM_POOL_SIZE));
    shell_print(sh, "ISR stack reserved:   %u bytes per CPU",
                static_cast<unsigned int>(CONFIG_ISR_STACK_SIZE));
    shell_print(sh, "RAM reservation includes stacks and heap; unreserved RAM is not free heap.");
    shell_print(sh, "");
    shell_print(sh, "Thread stacks (bytes; estimated peak usage since thread initialization):");
    shell_print(sh, "Address    Name              Total   Peak Unused  Use");

    // Shell output may block, so do not hold the thread-list spinlock here.
    k_thread_foreach_unlocked(print_thread_stack, const_cast<struct shell*>(sh));

    return 0;
}

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

    SHELL_CMD_ARG(
        info,
        NULL,
        "Display MCU and firmware information",
        cmd_sys_info,
        1,
        0
    ),

    SHELL_CMD_ARG(
        memory,
        NULL,
        "Display RAM reservation and thread stack usage",
        cmd_sys_memory,
        1,
        0
    ),

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
