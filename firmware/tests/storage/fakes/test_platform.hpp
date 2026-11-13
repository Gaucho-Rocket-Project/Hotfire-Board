#pragma once
// Host stand-ins for compiling the real SD logger without Zephyr hardware.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <sys/types.h>
#include <errno.h>

struct device {};
inline device test_device;
#define DEVICE_DT_GET(node) (&test_device)
inline bool device_is_ready(const device*) { return true; }
struct gpio_dt_spec {};
#define GPIO_DT_SPEC_GET(node, prop) {}
constexpr int GPIO_INPUT = 0;
inline bool gpio_is_ready_dt(const gpio_dt_spec*) { return true; }
inline int gpio_pin_configure_dt(const gpio_dt_spec*, int) { return 0; }
inline int gpio_pin_get_dt(const gpio_dt_spec*) { return 1; }
int entropy_get_entropy(const device*, uint8_t*, uint16_t);

struct k_mutex {};
#define K_MUTEX_DEFINE(name) k_mutex name
constexpr int K_FOREVER = -1, K_NO_WAIT = 0;
inline int k_mutex_lock(k_mutex*, int) { return 0; }
inline int k_mutex_unlock(k_mutex*) { return 0; }
inline int64_t k_uptime_get() { return 100; }
inline uint32_t k_cycle_get_32() { static uint32_t n; return ++n; }
inline uint64_t k_cyc_to_us_floor64(uint64_t cycles) { return cycles; }
inline void k_msleep(int) {}
#define K_THREAD_DEFINE(name, stack, entry, ...) [[maybe_unused]] auto name = &entry
struct k_msgq {
    size_t size;
    size_t capacity;
    std::deque<std::vector<uint8_t>> entries;
};
#define K_MSGQ_DEFINE(name, size, capacity, align) k_msgq name{size, capacity, {}}
inline int k_msgq_put(k_msgq* q, const void* data, int timeout) {
    if (timeout != K_NO_WAIT) { return -EINVAL; }
    if (q->entries.size() == q->capacity) { return -ENOMSG; }
    const auto* p = static_cast<const uint8_t*>(data);
    q->entries.emplace_back(p, p + q->size);
    return 0;
}
inline int k_msgq_get(k_msgq* q, void* data, int) {
    if (q->entries.empty()) { return -ENOMSG; }
    memcpy(data, q->entries.front().data(), q->size);
    q->entries.pop_front(); return 0;
}
inline size_t k_msgq_num_used_get(k_msgq* q) { return q->entries.size(); }
using atomic_t = int;
inline int atomic_get(const atomic_t* n) { return *n; }
inline int atomic_inc(atomic_t* n) { return (*n)++; }
#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...)
#define LOG_ERR(...)
#define LOG_LEVEL_INF 3
struct FATFS {};
using fs_mode_t = unsigned;
constexpr fs_mode_t FS_O_CREATE=1, FS_O_WRITE=2, FS_O_APPEND=4, FS_O_TRUNC=8;
constexpr int FS_SEEK_SET=0, FS_SEEK_END=2, FS_FATFS=1;
constexpr int FS_MOUNT_FLAG_NO_FORMAT=1, FS_MOUNT_FLAG_USE_DISK_ACCESS=2;
struct fs_mount_t { int type; const char* mnt_point; void* fs_data; int flags; };
struct fs_file_t { std::string path; size_t position=0; };
inline void fs_file_t_init(fs_file_t*) {}
int fs_open(fs_file_t*, const char*, fs_mode_t);
ssize_t fs_write(fs_file_t*, const void*, size_t);
int fs_sync(fs_file_t*);
int fs_close(fs_file_t*);
int fs_seek(fs_file_t*, off_t, int);
off_t fs_tell(fs_file_t*);
int fs_truncate(fs_file_t*, off_t);
int fs_mkdir(const char*);
int fs_mount(fs_mount_t*);
int fs_unmount(fs_mount_t*);
