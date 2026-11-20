#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "drivers/thermocouples.hpp"

struct ConfigError {
    size_t line = 0;
    char message[96]{};
};

int parse_thermocouple_config(
    const char* text,
    size_t length,
    ThermocoupleConfig& output,
    ConfigError& error);

#endif // APP_CONFIG_H