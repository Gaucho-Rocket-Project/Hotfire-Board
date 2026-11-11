cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hotfire_logger)

target_sources(app PRIVATE
    src/main.cpp
    src/bsp.cpp
    src/app/fsm.cpp
    # Drivers
    src/drivers/led.cpp
    src/drivers/uart.cpp
    src/drivers/sd.cpp
    src/drivers/thermocouples.cpp
    src/app/fsm.cpp
    # States
    src/app/state/init_state.cpp
    src/app/state/led_on_state.cpp
    src/app/state/led_off_state.cpp
    src/app/state/error_state.cpp
    # Tasks
    src/app/telemetry.cpp
    src/app/sensor_logging.cpp
    # Shell Commands
    src/shell/disk.cpp
    src/shell/daq.cpp
    src/shell/sys.cpp
)

target_include_directories(app PRIVATE
    include
)
