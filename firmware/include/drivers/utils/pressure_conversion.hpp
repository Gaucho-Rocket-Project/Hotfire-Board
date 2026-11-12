#ifndef DRIVERS_UTILS_PRESSURE_CONVERSION_HPP
#define DRIVERS_UTILS_PRESSURE_CONVERSION_HPP

#include <stdint.h>

namespace pressure_conversion {

// Each current-loop return is shunted to ground by 130 ohms.
// Microvolts / ohms = microamps. Preserve values outside 4..20 mA for diagnosis.
constexpr int32_t current_ua(int32_t voltage_uv)
{
    return (voltage_uv + 65) / 130;
}

} // namespace pressure_conversion
#endif
