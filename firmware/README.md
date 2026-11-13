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

## Pressure ADC display

Run `daq pressure` for one scan of all four pressure inputs, or `daq pressure_live` for the same display refreshed approximately once per second. Press **Enter** or **Ctrl+C** to exit the live view. It uses the same terminal requirements and alternate-screen behavior as `daq tc_live`.

Each channel shows its board label (P0–P3), connector, MCU/ADC pin, CSV column number, raw 12-bit ADC counts, and loop current in mA/uA using the 130 ohm shunt. The range field reports `BELOW 4 mA`, `WITHIN 4-20 mA`, or `ABOVE 20 mA`; it does not certify that a sensor is connected. Failed reads show `READ ERROR` while valid channels remain visible. Pressure in psi/bar is not displayed because the transducer ranges have not been configured.

These commands also work while logging is stopped. Each refresh performs an additional ADC scan; the ADC driver serializes it with logger reads. The display refresh rate does not change the 1 kHz pressure logging target. Check `daq log status` for missed periods while using diagnostics during a run.

## RTC and SD logging

Use a FAT-formatted SD card. At boot, the firmware initializes the sensors, mounts an inserted card, creates a fresh session directory, and starts independent temperature logging at a **10 Hz target** and pressure logging at a **1000 Hz target** after successful mounting. No shell command is required. Failed sensor channels produce blank fields while working channels continue logging. If the card is absent or mounting fails, automatic acquisition is skipped. Each successful eject/remount also starts a new directory, so changing cards cannot append to another session. Starting and stopping acquisition while the card stays mounted uses the same session.

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

`sd status` shows the session path and buffer counts; `daq log status` shows whether acquisition is running. Use `daq log stop` before ejecting the card, and `daq log start` to resume or start manually after inserting and mounting a card later. `sensor.csv` starts with `timestamp_utc,uptime_ms,sample_type,uptime_us,acquisition_us`, followed by the thermocouple and pressure columns. Each `temperature` row contains only temperature/fault data, and each `pressure` row contains only pressure data; the other group’s cells are blank. A frame contains readings for all four channels of its group, not one channel. Rows are queued in completion order; use `uptime_us` to order acquisition starts across the two workers. `system.log` contains application status events with UTC and uptime prefixes. Sensor timestamps are captured at acquisition and event timestamps at enqueue, so later SD writes preserve their original times. RTC timestamps have one-second resolution. `uptime_us` represents acquisition start with 100 µs kernel-tick resolution in this configuration; `uptime_ms` is retained for convenience. `acquisition_us` measures capture duration using the hardware cycle counter. FAT modification timestamps also use the RTC (two-second resolution, or 1980-01-01 when unset).

Sensor workers only enqueue into a 512-record RAM queue and never wait for SD I/O. A separate writer drains batches of up to 32 records, syncing once per batch; it retains an unsuccessful batch for retry. The queue holds about half a second at the combined 1010 frames/s target. If full, newly arriving records are dropped and counted, while acquisition continues. An additional 32-record batch may be in flight; system events retain a separate 64-event buffer. Records drain into a session when mounted, and RAM buffers do not survive reset or power loss. `daq log stop` waits for both acquisition workers and drains pending records if the card is mounted; use `sd eject` before removing the card.

## Sampling timing diagnostics

Run `daq log status` while logging, or after `daq log stop`, to see independent temperature and pressure measurements:

- **Target and measured frames/s:** the measured rate is `(frames - 1) / (last_start - first_start)` since the last start, not the configured timer frequency. It is zero until two frames exist and remains frozen after stopping. Failed frames still count as acquisition attempts; the separate all-channel-valid count shows how many had valid readings on all four channels.
- **Acquisition µs (last / average / maximum):** measured using unsigned `k_cycle_get_32()` differences and `k_cyc_to_us_floor64()`, including the timestamp capture and sensor reads. Subtraction handles a single cycle-counter wrap.
- **Capture + enqueue µs:** the same measurement through the nonblocking RAM enqueue. It excludes SD writes, statistics bookkeeping, and the wait for the next period.
- **Start interval µs:** the most recent and largest observed gap between frame starts. Nominal intervals are 100000 µs for temperature and 1000 µs for pressure.
- **Missed periods:** timer expirations coalesced while a worker was busy or delayed. Old slots are skipped instead of generating invented catch-up readings. **Work overruns** count capture/enqueue calls taking at least the target period; **queue drops** count records that could not be buffered.
- **SD diagnostics:** records successfully synced, pending records, cumulative drops/errors, and last/maximum batch formatting, writing, syncing, and closing duration. SD counters are cumulative since boot; per-stream statistics reset on `daq log start`.

The pressure worker has higher priority than temperature, UART command handling, the system workqueue, and SD writing. Periodic kernel timers schedule each stream independently, so temperature reads do not run inside the pressure acquisition loop. Temperature polling at 10 Hz does not guarantee a fresh MAX31856 conversion on every read; conversion settings and sensor faults still determine data availability.

The rates are **targets, not a claim of measured hardware performance**. Validate `daq log status` on the board with the intended SD card and other activity enabled. Missed periods and queue drops should remain zero for a loss-free run. The pressure input RC filter still limits analog bandwidth even though it is sampled at 1 kHz.

## Pressure transducer readings

Each pressure frame includes the four pressure inputs, sampled sequentially in one ADC1 sequence at 12-bit resolution. The target is 1000 four-channel frames per second (1000 readings per second per pressure channel), not 1000 readings shared among the four inputs. The CSV keeps the existing `p1_raw` through `p4_raw` columns and appends `p1_ua` through `p4_ua` for measured loop current in **microamps**.

| CSV channel | Board net | Connector (return on pin 3) | MCU pin / ADC1 channel | Shunt |
| --- | --- | --- | --- | --- |
| `p1_*` | P0 | J5 | PC0 / IN1 | R28, 130 Ω |
| `p2_*` | P1 | J9 | PC1 / IN2 | R35, 130 Ω |
| `p3_*` | P2 | J10 | PC2 / IN3 | R42, 130 Ω |
| `p4_*` | P3 | J7 | PC3 / IN4 | R29, 130 Ω |

The mapping follows the board schematic and the [STM32L496 datasheet](https://www.st.com/resource/en/datasheet/stm32l496re.pdf). The ADC uses a long acquisition time for the 4.7 kΩ / 0.1 µF input filters. Zephyr calibrates the ADC and estimates its reference rail from the factory-calibrated internal VREFINT at initialization, with 3300 mV as the driver's fallback reference.

Current is calculated as `shunt_voltage_microvolts / 130`, rounded to the nearest microamp. A 4 mA input produces 0.52 V; 20 mA produces 2.60 V. These are approximately 645 and 3227 raw counts with a 3.3 V reference. Values outside 4–20 mA are retained for diagnosis, including near-zero readings with no current loop attached. Successful ADC acquisition does **not** establish that a transducer is connected or that its pressure is valid.

Failed reads leave that channel's raw/current cells blank. `daq log status` reports the number of failed pressure-channel reads and the last error since acquisition started. Pressure ADC initialization failures are also recorded in `system.log`, and do not stop thermocouple logging. Likewise, pressure logging can run if thermocouple initialization fails.

Pressure in psi/bar requires each transducer's rated range; the 130 Ω resistor determines current conversion only. Once the sensor range is known, use `pressure = minimum + (current_ua - 4000) * (maximum - minimum) / 16000`. No pressure range is assumed by the firmware.

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

Replies end in CRLF. The existing `heartbeat` line continues once per second on USART1; the peer should ignore it while waiting for an `RTC` reply. Send one command at a time and retry after a timeout if startup or SD activity delays the response. USART1 uses DMA reception with two 128-byte buffers feeding a 512-byte software receive ring. A transmit queue keeps heartbeat and RTC replies in separate complete messages. Raw byte echo is replaced by the RTC command handler.

Successful UART time updates are recorded in `system.log`. They do not start sensor acquisition. Re-send time after power loss and periodically as needed to correct LSI clock drift.

## SPI and UART DMA

The overlay assigns independent DMA channels to SPI2 and USART1:

| Peripheral | Direction | DMA controller / channel | Request |
| --- | --- | --- | --- |
| SPI2 | RX | DMA1 / 4 | 1 |
| SPI2 | TX | DMA1 / 5 | 1 |
| USART1 | TX | DMA2 / 6 | 2 |
| USART1 | RX | DMA2 / 7 | 2 |

`CONFIG_DMA`, `CONFIG_SPI_STM32_DMA`, and `CONFIG_UART_ASYNC_API` enable these paths. The USB CDC shell continues using the interrupt UART API on its own device. USART1 exclusively uses the asynchronous API; do not also register an interrupt callback on USART1. See [Zephyr UART APIs](https://docs.zephyrproject.org/latest/hardware/peripherals/uart.html) and [ST's STM32L4 DMA overview](https://www.st.com/resource/en/product_training/stm32l4_system_dma.pdf).

SPI2 keeps the synchronous `spi_transceive()` interface: the calling thread waits for DMA completion while other threads can run. The thermocouple decoder selection and mutex remain held until the transaction finishes, keeping the existing stack-based transfer buffers valid. DMA does not increase the 1 MHz SPI clock or the MAX31856 conversion rate; setup and interrupt overhead can outweigh the benefit for short register reads.

`UART::write()` copies messages into an eight-entry queue (maximum 128 bytes each), with one additional persistent active DMA message. It returns immediately after queue admission: `0` means accepted, `-ENOSPC` means full, and `-EMSGSIZE` means too long. Transmission failures and rejected RTC replies/heartbeats are reported through Zephyr logging. Messages that fail to start or abort are dropped; the peer should discard an incomplete reply and retry after a timeout. The active buffer is retained until completion or abort, and the TX timeout is 100 ms.

RX uses a 1 ms idle timeout to deliver short commands and supplies the next DMA buffer before the current one fills. Released buffers are reused; a stopped/disabled receiver is restarted from workqueue context. Detected receive loss invalidates the RTC parser and produces `RTC ERR RX_OVERFLOW`. DMA does not prevent overflow if the peer sends faster than the command handler can consume.

After flashing, check `daq tc`/`daq tc_live` over USB, then send `RTC GET` and `RTC SET` over USART1 while SD logging runs. Check both streams in `daq log status`, especially measured Hz, maximum acquisition time, missed periods, and queue drops. Compare equivalent runs before and after DMA; CPU relief and achievable sampling rates still need measurement on the board. This change enables DMA for SPI2/USART1 only; ADC sampling and SDMMC configuration are unchanged.

## Host tests

Run the host checks from `firmware/`. These cover parsing, ADC scan mapping, timing arithmetic, separate CSV rows, queue overflow, retrying partial/failed SD batches, and UART DMA buffer ownership/recovery; they do not measure real-time hardware performance:

```bash
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/log_session_test.cpp -o /tmp/log_session_test
/tmp/log_session_test
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/rtc_protocol_test.cpp -o /tmp/rtc_protocol_test
/tmp/rtc_protocol_test
c++ -std=c++17 -Wall -Wextra -Werror -Itests/pressure/fakes -Iinclude tests/pressure_test.cpp src/drivers/pressure.cpp -o /tmp/pressure_test
/tmp/pressure_test
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/sampling_test.cpp -o /tmp/sampling_test
/tmp/sampling_test
c++ -std=c++17 -Wall -Wextra -Werror -Itests/storage/fakes -Iinclude tests/storage_batch_test.cpp src/drivers/sd.cpp -o /tmp/storage_batch_test
/tmp/storage_batch_test
c++ -std=c++17 -Wall -Wextra -Werror -Itests/uart/fakes -Iinclude tests/uart_dma_test.cpp src/drivers/uart.cpp -o /tmp/uart_dma_test
/tmp/uart_dma_test
```

## Troubleshooting

- **`west build` is unavailable or Zephyr cannot be found:** activate the Zephyr virtual environment, check `ZEPHYR_BASE`, and complete the workspace setup linked above.
- **Cache refers to another checkout:** run the pristine build command with `-d build`.
- **Programmer is missing or the probe cannot connect:** check the OpenOCD installation, USB connection, target power, common ground, and SWD wiring.
- **Flashing succeeds but LED5 does not blink:** reset the board, confirm `app.overlay` was included in the build output, and check the PB1/LED5 circuit.
