#include "app/config.hpp"

#include <errno.h>
#include <stdio.h>
#include <string_view>


namespace {

constexpr size_t kMaxConfigBytes = THERMOCOUPLE_CONFIG_MAX_BYTES;
constexpr size_t kChannelCount = Thermocouples::CHANNEL_COUNT;

std::string_view trim(std::string_view text)
{
    constexpr std::string_view whitespace{" \t\r", 3};

    const size_t first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }

    const size_t last = text.find_last_not_of(whitespace);
    return text.substr(first, last - first + 1);
}

bool parse_type(std::string_view text, ThermocoupleType& type)
{
    if (text.size() != 1) {
        return false;
    }

    switch (text.front()) {
    case 'B': type = ThermocoupleType::B; return true;
    case 'E': type = ThermocoupleType::E; return true;
    case 'J': type = ThermocoupleType::J; return true;
    case 'K': type = ThermocoupleType::K; return true;
    case 'N': type = ThermocoupleType::N; return true;
    case 'R': type = ThermocoupleType::R; return true;
    case 'S': type = ThermocoupleType::S; return true;
    case 'T': type = ThermocoupleType::T; return true;
    default: return false;
    }
}

} // namespace

int parse_thermocouple_config(
    const char* text,
    size_t length,
    ThermocoupleConfig& output,
    ConfigError& error)
{
    error = {};

    size_t line_number = 0;

    const auto fail = [&](const char* message, int code = -EINVAL) {
        error.line = line_number;
        snprintf(error.message, sizeof(error.message), "%s", message);
        return code;
    };

    if (text == nullptr || length == 0) {
        return fail("Configuration is empty");
    }

    if (length > kMaxConfigBytes) {
        return fail("Configuration exceeds 2048 bytes", -EFBIG);
    }

    ThermocoupleConfig candidate{};

    bool version_seen = false;
    bool section_seen[kChannelCount]{};
    bool enabled_seen[kChannelCount]{};
    bool type_seen[kChannelCount]{};

    // -1 denotes the global section before the first [tcN].
    int channel = -1;
    size_t offset = 0;

    while (offset < length) {
        ++line_number;

        const size_t start = offset;
        while (offset < length && text[offset] != '\n') {
            ++offset;
        }

        std::string_view line(text + start, offset - start);

        if (offset < length) {
            ++offset; // Skip newline.
        }

        // Check before removing comments so corruption is not ignored.
        if (line.find('\0') != std::string_view::npos) {
            return fail("Unexpected NUL byte in configuration");
        }

        const size_t comment = line.find_first_of("#;");
        if (comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[') {
            // Accept exactly [tc1], [tc2], [tc3], or [tc4].
            if (line.size() != 5 ||
                line[1] != 't' ||
                line[2] != 'c' ||
                line[3] < '1' ||
                line[3] > '4' ||
                line[4] != ']') {
                return fail("Expected section [tc1] through [tc4]");
            }

            channel = line[3] - '1';

            if (section_seen[channel]) {
                return fail("Duplicate thermocouple section");
            }

            section_seen[channel] = true;
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return fail("Expected key=value");
        }

        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));

        if (key.empty() || value.empty()) {
            return fail("Key and value must not be empty");
        }

        if (channel < 0) {
            if (key != "version") {
                return fail("Unknown global setting");
            }

            if (version_seen) {
                return fail("Duplicate version setting");
            }

            if (value != "1") {
                return fail("Unsupported configuration version");
            }

            version_seen = true;
            continue;
        }

        auto& settings = candidate.channels[channel];

        if (key == "enabled") {
            if (enabled_seen[channel]) {
                return fail("Duplicate enabled setting");
            }

            if (value == "true") {
                settings.enabled = true;
            } else if (value == "false") {
                settings.enabled = false;
            } else {
                return fail("enabled must be true or false");
            }

            enabled_seen[channel] = true;
        } else if (key == "type") {
            if (type_seen[channel]) {
                return fail("Duplicate type setting");
            }

            if (!parse_type(value, settings.type)) {
                return fail("type must be B, E, J, K, N, R, S, or T");
            }

            type_seen[channel] = true;
        } else {
            return fail("Unknown thermocouple setting");
        }
    }

    // Line 0 indicates a missing document-level requirement.
    line_number = 0;

    if (!version_seen) {
        return fail("Missing version=1 before thermocouple sections");
    }

    for (size_t ch = 0; ch < kChannelCount; ++ch) {
        if (!section_seen[ch] ||
            !enabled_seen[ch] ||
            !type_seen[ch]) {
            error.line = 0;
            snprintf(
                error.message,
                sizeof(error.message),
                "TC%u requires a section with enabled and type",
                static_cast<unsigned>(ch + 1));
            return -EINVAL;
        }
    }

    output = candidate;
    return 0;
} 