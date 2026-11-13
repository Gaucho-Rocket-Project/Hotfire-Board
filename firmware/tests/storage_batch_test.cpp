#include "drivers/sd.hpp"
#include "drivers/rtc.hpp"
#include "drivers/utils/sample_csv.hpp"
#include "test_platform.hpp"

#include <cassert>
#include <map>
#include <set>
#include <algorithm>

namespace {
std::map<std::string, std::string> files;
std::set<std::string> directories;
unsigned fs_calls = 0;
bool short_write = false;
bool fail_sync = false;
bool fail_close = false;
unsigned entropy = 0;
}

rtc_clock::Timestamp rtc_clock::now() { return {}; }
int entropy_get_entropy(const device*, uint8_t* bytes, uint16_t length)
{
    memset(bytes, 0, length);
    bytes[0] = ++entropy;
    return 0;
}

int fs_mkdir(const char* path)
{
    ++fs_calls;
    return directories.insert(path).second ? 0 : -EEXIST;
}
int fs_mount(fs_mount_t*) { ++fs_calls; return 0; }
int fs_unmount(fs_mount_t*) { ++fs_calls; return 0; }
int fs_open(fs_file_t* f, const char* path, fs_mode_t mode)
{
    ++fs_calls;
    f->path = path;
    auto& data = files[path];
    if (mode & FS_O_TRUNC) {
        data.clear();
    }
    f->position = mode & FS_O_APPEND ? data.size() : 0;
    return 0;
}
ssize_t fs_write(fs_file_t* f, const void* bytes, size_t length)
{
    ++fs_calls;
    size_t n = length;
    if (short_write) {
        n /= 2;
        short_write = false;
    }
    auto& data = files.at(f->path);
    if (data.size() < f->position + n) {
        data.resize(f->position + n);
    }
    data.replace(f->position, n, static_cast<const char*>(bytes), n);
    f->position += n;
    return n;
}
int fs_sync(fs_file_t*)
{
    ++fs_calls;
    if (fail_sync) { fail_sync = false; return -EIO; }
    return 0;
}
int fs_close(fs_file_t*)
{
    ++fs_calls;
    if (fail_close) { fail_close = false; return -EIO; }
    return 0;
}
int fs_seek(fs_file_t* f, off_t offset, int origin)
{
    ++fs_calls;
    f->position = (origin == FS_SEEK_END ? files.at(f->path).size() : 0) + offset;
    return 0;
}
off_t fs_tell(fs_file_t* f) { return f->position; }
int fs_truncate(fs_file_t* f, off_t length)
{
    ++fs_calls;
    files.at(f->path).resize(length);
    return 0;
}

int main()
{
    storage::SensorSample sample;
    sample.type = storage::SampleType::Pressure;
    sample.pressure_valid_mask = 15;
    sample.pressure_raw[0] = 1234;
    assert(storage::append_startup_record() == 0);
    const unsigned before_enqueue = fs_calls;
    for (unsigned i = 0; i < 512; ++i) {
        sample.uptime_us = i * 1000;
        assert(storage::append_sensor_sample(sample) == 0);
    }
    assert(storage::append_sensor_sample(sample) == -ENOSPC);
    assert(fs_calls == before_enqueue); // Enqueue never performs SD work, even on overflow.
    assert(storage::get_status().pending_samples == 512);
    assert(storage::get_status().dropped_samples == 1);

    assert(storage::mount_sd() == 0);
    auto status = storage::get_status();
    assert(status.pending_samples == 480 && status.written_samples == 32);
    assert(storage::flush_pending() == 0);
    status = storage::get_status();
    assert(status.pending_samples == 0 && status.written_samples == 512);
    const std::string path = std::string(status.session_directory) + "/sensor.csv";
    std::string expected = storage::CSV_HEADER;
    for (unsigned i = 0; i < 512; ++i) {
        sample.uptime_us = i * 1000;
        char row[storage::CSV_LINE_SIZE];
        const int n = storage::format_sensor_csv(sample, row, sizeof(row));
        expected.append(row, n);
    }
    assert(files.at(path) == expected); // No reordering, missing rows or duplicated headers.

    sample.type = storage::SampleType::Temperature;
    sample.pressure_valid_mask = 0;
    sample.cj_valid_mask = 1;
    for (unsigned i = 0; i < 4; ++i) {
        sample.uptime_us = 2000000 + i * 100000;
        assert(storage::append_sensor_sample(sample) == 0);
        char row[storage::CSV_LINE_SIZE];
        const int n = storage::format_sensor_csv(sample, row, sizeof(row));
        expected.append(row, n);
    }
    short_write = true;
    assert(storage::flush_pending() == -EIO);
    assert(storage::get_status().pending_samples == 4);
    fail_sync = true;
    assert(storage::flush_pending() == -EIO);
    fail_close = true;
    assert(storage::flush_pending() == -EIO);
    assert(storage::get_status().written_samples == 512);
    assert(storage::get_status().write_errors == 3);
    assert(storage::flush_pending() == 0);
    assert(files.at(path) == expected); // Retried uncertain writes must not duplicate rows.
    assert(storage::get_status().written_samples == 516);

    assert(storage::unmount_sd() == 0);
    assert(storage::append_sensor_sample(sample) == 0);
    assert(storage::mount_sd() == 0);
    const std::string next = std::string(storage::get_status().session_directory) + "/sensor.csv";
    assert(next != path);
    assert(files.at(path) == expected);
    assert(std::count(files.at(next).begin(), files.at(next).end(), '\n') == 2);
    assert(storage::get_status().pending_samples == 0);
}
