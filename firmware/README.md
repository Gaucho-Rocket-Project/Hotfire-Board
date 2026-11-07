# DAQ Firmware

Zephyr bring-up firmware for the Gaucho Rocket Project data logger. The application toggles **LED5 (PB1, active high) every 500 ms**, giving a one-second on/off cycle. Serial output is disabled. Sensor acquisition, microSD logging, and telemetry are not implemented yet.

## Target and source files

The build uses Zephyr's `nucleo_l496zg` target as a starting point for the custom STM32L496RE board. [app.overlay](app.overlay) selects PB1 for `led0`, limits flash to **512 KiB**, and disables Nucleo peripherals unused by this application. Keep this overlay enabled when building for the DAQ board.

- [src/main.c](src/main.c): GPIO initialization and blink loop.
- [prj.conf](prj.conf): GPIO support enabled; serial and console support disabled.
- [CMakeLists.txt](CMakeLists.txt): Zephyr application build definition.

## Prerequisites

Set up a Zephyr workspace, Python virtual environment with `west`, host build tools (including CMake and Ninja), and the Zephyr SDK with its ARM toolchain using the [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html). Complete the workspace update and Python dependency installation steps before building this application.

The local build environment uses **Zephyr 4.5.99**, **Zephyr SDK 1.0.1**, and **west 1.5.0**. This repository does not pin a Zephyr revision or include a west manifest.

For flashing, use an **ST-LINK probe** and either **OpenOCD** or **STM32CubeProgrammer**. These runners are provided by the inherited [Nucleo target](https://docs.zephyrproject.org/latest/boards/st/nucleo_l496zg/doc/index.html).

## Build

The following commands use a Bash-compatible shell on macOS/Linux and assume the Zephyr workspace is at `~/zephyrproject`. Adjust the workspace, SDK, and repository paths for your machine.

```bash
source "$HOME/zephyrproject/.venv/bin/activate"
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"

cd /path/to/daq-module/firmware
west build -p always -b nucleo_l496zg -d build-local .
```

Zephyr automatically loads `prj.conf` and `app.overlay`. The `-p always` option starts a pristine build. Use `build-local` because the checked-in `build/` directory contains generated files with paths from another checkout; do not rely on those cached files or binaries. Keep your generated build directory out of commits.

For subsequent source changes:

```bash
west build -d build-local .
```

Outputs are in `build-local/zephyr/`: `zephyr.elf`, `zephyr.hex`, and `zephyr.bin`.

## Connect and flash

Power the DAQ board, then connect the ST-LINK to the board's SWD header using the signal names in the [schematic](../hotfire-board/Hotfire-Board.kicad_sch):

| ST-LINK signal | DAQ board signal |
| --- | --- |
| Target voltage reference | 3.3 V |
| GND | GND |
| SWDIO | SWDIO |
| SWCLK | SWCLK |
| NRST | NRST |

The voltage-reference connection senses the target supply; power the board separately. Connect the probe to your computer by USB. The DAQ board's USB-C connector alone does not provide an ST-LINK debug connection.

From `firmware/`, with the same environment active, flash using OpenOCD:

```bash
west flash -d build-local --runner openocd
```

Alternatively, with `STM32_Programmer_CLI` installed and on `PATH`:

```bash
west flash -d build-local --runner stm32cubeprogrammer
```

The STM32CubeProgrammer runner uses SWD and hardware reset, so connect NRST. Both commands program the application into internal flash. After flashing and reset, LED5 should change state every half second. No UART output or USB serial device is expected from this firmware.

## Troubleshooting

- **`west build` is unavailable or Zephyr cannot be found:** activate the Zephyr virtual environment, check `ZEPHYR_BASE`, and complete the workspace setup linked above.
- **Cache refers to another checkout:** run the pristine build command with `-d build-local`.
- **Programmer is missing or the probe cannot connect:** check the selected runner's installation, USB connection, target power, common ground, SWD wiring, and NRST.
- **Flashing succeeds but LED5 does not blink:** reset the board, confirm `app.overlay` was included in the build output, and check the PB1/LED5 circuit.
