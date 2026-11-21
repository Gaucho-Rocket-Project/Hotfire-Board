# DAQ Firmware

Zephyr firmware for the Gaucho Rocket Project data logger, with a USB shell, thermocouple, pressure, and ADXL345 acquisition, and RTC-timestamped microSD logging. The application toggles **LED5 (PB1, active high) every 500 ms**, giving a one-second on/off cycle.

## Target and source files

The build uses Zephyr's `nucleo_l496zg` target as a starting point for the custom STM32L496RE board. [app.overlay](app.overlay) selects PB1 for `led0`, limits flash to **512 KiB**, and disables Nucleo peripherals unused by this application. Keep this overlay enabled when building for the DAQ board.

- [src/main.cpp](src/main.cpp): application initialization and main loop.
- [src/bsp.cpp](src/bsp.cpp) and [include/bsp.hpp](include/bsp.hpp): application board mappings for the LED, UART, shared sensor SPI settings and decoder GPIOs, SD card detect, pressure ADC inputs, expansion GPIOs, RTC, entropy device, and memory sizes. Drivers and shell commands consume these definitions. Peripheral pinctrl, DMA routing, and devicetree properties remain in [app.overlay](app.overlay).
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

Select bring-up channels with `ENABLED_CHANNEL_MASK` in [include/drivers/thermocouples.hpp](include/drivers/thermocouples.hpp): bit 0 is TC1 through bit 3 for TC4. Use `0x07` for TC1-TC3, `0x08` for TC4 only, or `0x0f` for all four, then rebuild. Disabled channels receive no configuration or reads, display `DISABLED`, and have blank CSV fields. Temperature frame validity checks only the enabled channels.

Startup puts each enabled MAX31856 into standby/interrupt fault mode and clears retained faults before configuring Type K and restarting continuous conversions. The running CR0 value remains `0x90`: 60 Hz rejection, internal cold-junction sensing, comparator fault mode, and open-circuit detection with the timing for series resistance below 5 kohm. This matches the board BOM's 100 ohm input resistors, 100 nF differential capacitor and 10 nF capacitors to ground. See the [MAX31856 datasheet, Table 4 and CR0 register](https://www.analog.com/media/en/technical-documentation/data-sheets/max31856.pdf).

The firmware reports startup fault codes after a 200 ms conversion wait. Input over/undervoltage (`0x02`) suspends conversions; cold-junction reads then return an error rather than treating a stale zero as a valid temperature. The shell labels this fault `INPUT OVER/UNDERVOLTAGE`. Open-circuit detection and fault clearing cannot guarantee recovery from an electrical input-bias problem. A disconnected probe may still require hardware diagnosis; the startup sequence has not been validated on the board.

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

`sd status` shows the session path and buffer counts; `daq log status` shows whether acquisition is running. Use `daq log stop` before ejecting the card, and `daq log start` to resume or start manually after inserting and mounting a card later. `sensor.csv` starts with `timestamp_utc,uptime_ms,sample_type,uptime_us,acquisition_us`, followed by thermocouple, pressure, and acceleration columns. Each `temperature` row contains only temperature/fault data, and each `pressure` row contains only pressure data; the other group’s cells are blank. A frame contains readings for all four channels of its group, not one channel. Rows are queued in completion order; use `uptime_us` to order acquisition starts across the three workers. `system.log` contains application status events with UTC and uptime prefixes. Sensor timestamps are captured at acquisition and event timestamps at enqueue, so later SD writes preserve their original times. RTC timestamps have one-second resolution. `uptime_us` represents acquisition start with 100 µs kernel-tick resolution in this configuration; `uptime_ms` is retained for convenience. `acquisition_us` measures capture duration using the hardware cycle counter. FAT modification timestamps also use the RTC (two-second resolution, or 1980-01-01 when unset).

Sensor workers only enqueue into a 512-record RAM queue and never wait for SD I/O. A separate writer drains batches of up to 32 records, syncing once per batch; it retains an unsuccessful batch for retry. The queue holds about 0.46 seconds at the combined 1110 frames/s target. If full, newly arriving records are dropped and counted, while acquisition continues. An additional 32-record batch may be in flight; system events retain a separate 64-event buffer. Records drain into a session when mounted, and RAM buffers do not survive reset or power loss. `daq log stop` waits for all active acquisition workers and drains pending records if the card is mounted; use `sd eject` before removing the card.

## ADXL345 acceleration

Connect the ADXL345 to SPI2 (SCK PB13, MISO PB14, MOSI PB15), ground, an appropriate
3.3 V supply, and **CS13 / decoder Y13**. Thermocouples remain on Y0-Y3.
Y4-Y7 and Y15 must remain unconnected: they are parking outputs used to select
and deselect sensors without passing through another populated chip select.
The decoder changes one address bit at a time; Y13 is selected with A3:A0=1101.
Y15 remains the idle selection. Only Y0-Y3 and Y13 are accepted by the shared
SPI decoder interface.
No interrupt pin is required by this polling implementation.

The driver verifies device ID `0xE5` and configuration readback, then runs at
**100 Hz, full resolution, +/-16 g, four-wire SPI mode 3 at 1 MHz**. Thermocouples
also use mode 3, keeping SCK polarity consistent between sensor types. This is
a diagnostic change for TC readback failures with the accelerometer connected;
hardware validation is still required, and it does not guarantee prevention of
the ADXL345's documented accidental I2C recognition on a shared SPI bus.
Both drivers use `spi_decoder.cpp`, whose mutex covers address
selection, transfer completion, and deselection. A dummy transfer at idle Y15
applies each SPI configuration before asserting the next sensor's CS. SPI and GPIO mappings
remain in the BSP. See the [ADXL345 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/adxl345.pdf)
for register definitions and FIFO behavior.

A separate priority-3 worker polls the stream FIFO every 10 ms and drains at most
32 entries per wake. Pressure remains priority 1, temperature priority 5, and the
SD writer priority 7. The FIFO buffers approximately 320 ms of data. It is cleared
before each recording starts, so idle-period samples do not enter a new recording.
An absent or misconfigured accelerometer does not prevent temperature/pressure
logging. An empty FIFO produces no row; SPI read errors are counted rather than
being converted into zero acceleration. FIFO full events are counted separately;
they indicate possible lost data, not an exact number of lost samples.

Each `acceleration` row appends `accel_x_raw,accel_y_raw,accel_z_raw,accel_x_mg,accel_y_mg,accel_z_mg,accel_fifo_overrun`
to the CSV schema. `mg` means milligravity (1/1000 g), using the nominal full-resolution
scale of 4 mg/LSB; raw counts are retained for calibration. Other sensor fields are
blank in acceleration rows, and acceleration fields are blank in other rows.
`uptime_us` is the **host FIFO-read start time**, not a hardware conversion timestamp.
After a delayed poll, buffered records can have closely spaced read timestamps.
There is no assumed synchronization between the accelerometer's clock and the MCU.
The TC-only SD configuration file is unchanged; accelerometer settings are currently
fixed in the driver.

Use `daq accel` to see the latest acquired sample, including its host uptime.
Use `daq accel_live` for the same raw XYZ and milligravity display refreshed
approximately once per second; press **Enter** or **Ctrl+C** to return to the shell.
It uses the same alternate-screen behavior as `daq tc_live`. Both acceleration
commands use cached samples without consuming FIFO entries or issuing SPI reads.
Acquisition must be running (`daq log start`) for fresh samples; a stopped worker
is identified in the display. The display refresh rate does not change the sensor
sampling rate. `daq log status` adds acceleration timing, read errors, queue drops,
FIFO full events, and empty-poll counts. Acceleration frames count actual FIFO
records; temperature/pressure frames count attempts. Verify pressure timing and
queue drops with all sensors running before relying on the requested rates.

## Sampling timing diagnostics

Run `daq log status` while logging, or after `daq log stop`, to see independent temperature, pressure, and acceleration measurements:

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

SPI2 keeps the synchronous `spi_transceive()` interface: the calling thread waits for DMA completion while other threads can run. The shared SPI decoder selection and mutex remain held until the transaction finishes, keeping the existing stack-based transfer buffers valid. DMA does not increase the 1 MHz SPI clock or the MAX31856 conversion rate; setup and interrupt overhead can outweigh the benefit for short register reads.

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
c++ -std=c++17 -pthread -Wall -Wextra -Werror -Itests/thermocouples/fakes -Iinclude tests/thermocouples_test.cpp src/drivers/thermocouples.cpp src/drivers/spi_decoder.cpp -o /tmp/thermocouples_test
/tmp/thermocouples_test
c++ -std=c++17 -pthread -Wall -Wextra -Werror -Itests/thermocouples/fakes -Iinclude tests/adxl345_test.cpp src/drivers/adxl345.cpp src/drivers/spi_decoder.cpp -o /tmp/adxl345_test
/tmp/adxl345_test
c++ -std=c++17 -pthread -Wall -Wextra -Werror -Itests/workers/fakes -Iinclude tests/sampling_workers_test.cpp src/app/sensor_logging.cpp -o /tmp/sampling_workers_test
/tmp/sampling_workers_test
c++ -std=c++17 -Wall -Wextra -Werror -Itests/storage/fakes -Iinclude tests/sampling_test.cpp -o /tmp/sampling_test
/tmp/sampling_test
c++ -std=c++17 -Wall -Wextra -Werror -Itests/storage/fakes -Iinclude tests/storage_batch_test.cpp src/drivers/sd.cpp -o /tmp/storage_batch_test
/tmp/storage_batch_test
c++ -std=c++17 -Wall -Wextra -Werror -Itests/uart/fakes -Iinclude tests/uart_dma_test.cpp src/drivers/uart.cpp -o /tmp/uart_dma_test
/tmp/uart_dma_test
```

## Troubleshooting

If `daq tc` reports a startup failure, it now prints the retained original error,
failed operation, channel/decoder address, and any CR0/CR1 readback values. This
status remains available even if USB boot messages were dropped. A failed startup
leaves the whole TC group unavailable; it does not mean every enabled amplifier
independently failed. `Fault: UNAVAILABLE (read failed)` means the fault register
was not read, rather than a verified zero fault. Startup details are also written
to `system.log`. Deferred core and USB-shell log buffers are 4096 bytes each.


- **`west build` is unavailable or Zephyr cannot be found:** activate the Zephyr virtual environment, check `ZEPHYR_BASE`, and complete the workspace setup linked above.
- **Cache refers to another checkout:** run the pristine build command with `-d build`.
- **Programmer is missing or the probe cannot connect:** check the OpenOCD installation, USB connection, target power, common ground, and SWD wiring.
- **Flashing succeeds but LED5 does not blink:** reset the board, confirm `app.overlay` was included in the build output, and check the PB1/LED5 circuit.
