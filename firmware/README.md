# DAQ Firmware

Zephyr firmware for the Gaucho Rocket Project data logger, with a USB shell, thermocouple acquisition, and RTC-timestamped microSD logging. The application toggles **LED5 (PB1, active high) every 500 ms**, giving a one-second on/off cycle.

## Target and source files

The build uses Zephyr's `nucleo_l496zg` target as a starting point for the custom STM32L496RE board. [app.overlay](app.overlay) selects PB1 for `led0`, limits flash to **512 KiB**, and disables Nucleo peripherals unused by this application. Keep this overlay enabled when building for the DAQ board.

- [src/main.cpp](src/main.cpp): application initialization and main loop.
- [src/drivers/sd.cpp](src/drivers/sd.cpp): buffered sensor and system logging.
- [src/drivers/rtc.cpp](src/drivers/rtc.cpp): RTC timestamps for logs and FAT metadata.
- [prj.conf](prj.conf): peripheral, filesystem, and USB shell configuration.
- [CMakeLists.txt](CMakeLists.txt): Zephyr application build definition.

## Prerequisites

Set up a Zephyr workspace, Python virtual environment with `west`, host build tools (including CMake and Ninja), and the Zephyr SDK with its ARM toolchain using the [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html). Complete the workspace update and Python dependency installation steps before building this application.

The local build environment uses **Zephyr 4.5.99**, **Zephyr SDK 1.0.1**, and **west 1.5.0**. This repository does not pin a Zephyr revision or include a west manifest.

For flashing, use an **ST-LINK probe** and **OpenOCD**, with `openocd` available on `PATH`.

## Build

The following commands use a Bash-compatible shell on macOS/Linux and assume the Zephyr workspace is at `~/zephyrproject`. Adjust the workspace, SDK, and repository paths for your machine.

```bash
source "$HOME/zephyrproject/.venv/bin/activate"
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"

cd /path/to/daq-module/firmware
west build -p always -b nucleo_l496zg -d build .
```

Zephyr automatically loads `prj.conf` and `app.overlay`. The `-p always` option starts a pristine build, regenerating any cached paths from another checkout. Rebuild locally before flashing; do not rely on checked-in binaries. Keep generated build files out of commits.

For subsequent source changes:

```bash
west build -d build .
```

Outputs are in `build/zephyr/`: `zephyr.elf`, `zephyr.hex`, and `zephyr.bin`.

## Connect and flash

Power the DAQ board, then connect the ST-LINK to the board's SWD header using the signal names in the [schematic](../hotfire-board/Hotfire-Board.kicad_sch):

| ST-LINK signal | DAQ board signal |
| --- | --- |
| Target voltage reference | 3.3 V |
| GND | GND |
| SWDIO | SWDIO |
| SWCLK | SWCLK |
| NRST | Optional; unused by the command below |

The voltage-reference connection senses the target supply; power the board separately. Connect the probe to your computer by USB. The DAQ board's USB-C connector alone does not provide an ST-LINK debug connection.

From `firmware/`, after building, flash using OpenOCD:

```bash
openocd \
  -f interface/stlink.cfg \
  -f target/stm32l4x.cfg \
  -c "reset_config none" \
  -c "init" \
  -c "halt" \
  -c "flash write_image erase build/zephyr/zephyr.hex" \
  -c "verify_image build/zephyr/zephyr.hex" \
  -c "reset run" \
  -c "shutdown"
```

This command uses `reset_config none`, so no physical reset-line connection is required. It halts the MCU, writes and verifies the HEX image, then resets the MCU to run the application and exits OpenOCD. After flashing and reset, LED5 should change state every half second. Connect the board's USB-C port to access the `daq:~$` shell.

Flash over usb:
```bash
dfu-util \
  -a 0 \
  -s 0x08000000 \
  -D build/zephyr/zephyr.bin
```

## Live thermocouple display

Run `daq tc` for a single reading, or `daq tc_live` for a continuously refreshed display of all four channels. The live view shows the same internal temperature, thermocouple temperature/connection status, and fault fields, refreshing approximately once per second. Press **Enter** or **Ctrl+C** to stop and return to the shell prompt.

Use an ANSI/VT100-compatible terminal (such as `screen`) with at least 26 rows and 60 columns. The live view uses the alternate screen, preserving the previous terminal contents when you exit. Sensor acquisition/logging and USART1 RTC commands continue running; exit the live view before entering another USB shell command.

## RTC and SD logging

Use a FAT-formatted SD card. At boot, the firmware mounts an inserted card, creates a fresh session directory, and immediately starts sensor logging at 1 Hz after successful sensor initialization and mounting. No shell command is required. If the card is absent or mounting fails, automatic acquisition is skipped. Each successful eject/remount also starts a new directory, so changing cards cannot append to another session. Starting and stopping acquisition while the card stays mounted uses the same session.

```text
/SD:/6e2a70bc59384ad1b726e08c4f9153da/
    sensor.csv
    system.log
```

The directory name is a 128-bit random identifier rendered as 32 hexadecimal characters, generated by the STM32 hardware RNG. Directory creation checks for collisions and generates another ID if one already exists; it never reuses a previous session. RNG or filesystem failures leave records buffered for retry. Existing directories and older root-level logs are left intact. Folder uniqueness does not depend on the RTC being set.

Set the hardware RTC to the **current UTC time** through the USB shell. For example (replace the date/time):

```text
rtc set rtc@40002800 2026-11-11T18:30:45
rtc get rtc@40002800
```

Use Tab after `rtc get ` to complete the device name if needed. The `date` command controls a separate system clock; use `rtc set` for these logs. The inherited board configuration uses the internal LSI oscillator, so check the clock periodically for drift and after power loss.

If the RTC is unavailable or unset, records use `UNSET` instead of an invented date. Setting the RTC affects subsequent records; the session folder keeps its unique ID and already buffered records retain their original timestamps.

```text
sd status
daq log start
daq log stop
sd eject
sd mount
```

`sd status` shows the session path and buffer counts; `daq log status` shows whether acquisition is running. Use `daq log stop` before ejecting the card, and `daq log start` to resume or start manually after inserting and mounting a card later. `sensor.csv` starts with `timestamp_utc,uptime_ms` followed by the thermocouple and pressure columns. `system.log` contains application status events with UTC and uptime prefixes. Sensor timestamps are captured at acquisition and event timestamps at enqueue, so later SD writes preserve their original times. RTC timestamps have one-second resolution; uptime remains in milliseconds. FAT modification timestamps also use the RTC (two-second resolution, or 1980-01-01 when unset).

Records queue in RAM while the card is unavailable, subject to the existing buffer limits (64 events and 128 samples). They are flushed into the new session when mounted; RAM buffers do not survive a reset. Stop logging and use `sd eject` before removing the card.

## Set the RTC from another board

Connect the other board's TX to **USART1 RX (PA10)**, its RX to **USART1 TX (PA9)**, and share ground. Use **3.3 V UART, 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control**. This interface works independently of the USB shell.

Commands are case-sensitive ASCII lines terminated by LF (`\n`); CRLF (`\r\n`) is also accepted. Set the **current UTC time** with a required trailing `Z`. Example exchange:

```text
Other board -> DAQ: RTC SET 2026-11-11T18:30:45Z\n
DAQ -> Other board: RTC OK 2026-11-11T18:30:45Z\r\n

Other board -> DAQ: RTC GET\n
DAQ -> Other board: RTC TIME 2026-11-11T18:30:46Z\r\n
```

The `SET` acknowledgment confirms that the RTC driver accepted the supplied time. `GET` reads the current hardware calendar, or replies `RTC TIME UNSET` when it cannot read a valid time. Supported years are 2000–2099; invalid dates, leap days, times, trailing text, and oversized commands are rejected without changing the RTC.

| Reply | Meaning |
| --- | --- |
| `RTC ERR INVALID_COMMAND` | Malformed, unknown, or overlong command; maximum line length is 47 bytes before LF, including optional CR. |
| `RTC ERR SET_FAILED <negative errno>` | The RTC device could not accept the update. |
| `RTC ERR RX_OVERFLOW` | UART data was lost or had a receive error. Send a blank LF to resynchronize, then retry the command. |

Replies end in CRLF. The existing `heartbeat` line continues once per second on USART1; the peer should ignore it while waiting for an `RTC` reply. Send one command at a time and retry after a timeout if startup or SD activity delays the response. UART reception is interrupt-buffered (256 bytes), and transmit locking prevents heartbeat bytes from splitting a reply. Raw byte echo is replaced by the RTC command handler.

Successful UART time updates are recorded in `system.log`. They do not start sensor acquisition. Re-send time after power loss and periodically as needed to correct LSI clock drift.

## Host tests

Check session collision/error handling and UART command/date parsing, from `firmware/`:

```bash
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/log_session_test.cpp -o /tmp/log_session_test
/tmp/log_session_test
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/rtc_protocol_test.cpp -o /tmp/rtc_protocol_test
/tmp/rtc_protocol_test
```

## Troubleshooting

- **`west build` is unavailable or Zephyr cannot be found:** activate the Zephyr virtual environment, check `ZEPHYR_BASE`, and complete the workspace setup linked above.
- **Cache refers to another checkout:** run the pristine build command with `-d build`.
- **Programmer is missing or the probe cannot connect:** check the OpenOCD installation, USB connection, target power, common ground, and SWD wiring.
- **Flashing succeeds but LED5 does not blink:** reset the board, confirm `app.overlay` was included in the build output, and check the PB1/LED5 circuit.
