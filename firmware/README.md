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

This command uses `reset_config none`, so no physical reset-line connection is required. It halts the MCU, writes and verifies the HEX image, then resets the MCU to run the application and exits OpenOCD. After flashing and reset, LED5 should change state every half second. No UART output or USB serial device is expected from this firmware.

Flash over usb:

- plug into usb
- hold BOOT
- while holding BOOT, press RESET
- Release BOOT

verify if flash is listed:
```bash
dfu-util -l
```

Flash with:
```bash
dfu-util \
  -a 0 \
  -s 0x08000000 \
  -D build/zephyr/zephyr.bin
```

## Troubleshooting

- **`west build` is unavailable or Zephyr cannot be found:** activate the Zephyr virtual environment, check `ZEPHYR_BASE`, and complete the workspace setup linked above.
- **Cache refers to another checkout:** run the pristine build command with `-d build`.
- **Programmer is missing or the probe cannot connect:** check the OpenOCD installation, USB connection, target power, common ground, and SWD wiring.
- **Flashing succeeds but LED5 does not blink:** reset the board, confirm `app.overlay` was included in the build output, and check the PB1/LED5 circuit.
