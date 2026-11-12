#ifndef DRIVERS_UTILS_LOG_SESSION_HPP
#define DRIVERS_UTILS_LOG_SESSION_HPP

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

namespace storage {

// Owned by the SD logger and accessed under its mutex. mkdir must return
// -EEXIST for an existing file/directory; existing sessions are never reused.
struct LogSession {
    char directory[40]{};
    char system_path[64]{};
    char sensor_path[64]{};

    int create(int (*random_bytes)(uint8_t*, size_t), int (*mkdir)(const char*))
    {
        if (directory[0] != '\0') {
            return 0; // Retry file initialization in the already reserved session.
        }

        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            uint8_t id[16];
            int ret = random_bytes(id, sizeof(id));
            if (ret < 0) {
                return ret;
            }
            char candidate[sizeof(directory)];
            constexpr char hex[] = "0123456789abcdef";
            memcpy(candidate, "/SD:/", 5);
            for (size_t i = 0; i < sizeof(id); ++i) {
                candidate[5 + 2 * i] = hex[id[i] >> 4];
                candidate[6 + 2 * i] = hex[id[i] & 0x0f];
            }
            candidate[37] = '\0';
            ret = mkdir(candidate);
            if (ret == -EEXIST) {
                continue;
            }
            if (ret < 0) {
                return ret;
            }

            snprintf(directory, sizeof(directory), "%s", candidate);
            snprintf(system_path, sizeof(system_path), "%s/system.log", directory);
            snprintf(sensor_path, sizeof(sensor_path), "%s/sensor.csv", directory);
            return 0;
        }
        return -EEXIST; // Repeated collisions: fail rather than reuse an old log.
    }
};

} // namespace storage

#endif
