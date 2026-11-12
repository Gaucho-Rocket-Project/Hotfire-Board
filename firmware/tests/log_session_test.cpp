// From firmware/: c++ -std=c++17 -Wall -Wextra -Werror -Iinclude
// tests/log_session_test.cpp -o /tmp/log_session_test && /tmp/log_session_test
#include "drivers/utils/log_session.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::string root;
int mkdir_error = 0;
int random_error = 0;
unsigned next_id = 0;
bool repeat_id = false;
unsigned mkdir_calls = 0;

std::string local_path(const char* path)
{
    assert(std::string(path).rfind("/SD:/", 0) == 0);
    return root + "/" + (path + 5);
}

int make_directory(const char* path)
{
    ++mkdir_calls;
    if (mkdir_error) {
        return mkdir_error;
    }
    return mkdir(local_path(path).c_str(), 0700) == 0 ? 0 : -errno;
}

int random_bytes(uint8_t* bytes, size_t length)
{
    if (random_error) {
        return random_error;
    }
    assert(length == 16);
    memset(bytes, 0, length);
    bytes[15] = next_id;
    if (!repeat_id) {
        ++next_id;
    }
    return 0;
}
} // namespace

int main()
{
    char temp[] = "/tmp/daq-log-session-XXXXXX";
    const char* created = mkdtemp(temp);
    assert(created);
    root = created;

    storage::LogSession first;
    assert(first.create(random_bytes, make_directory) == 0);
    assert(std::string(first.directory) == "/SD:/00000000000000000000000000000000");
    assert(std::string(first.sensor_path) == std::string(first.directory) + "/sensor.csv");
    assert(std::string(first.system_path) == std::string(first.directory) + "/system.log");
    std::ofstream(local_path(first.sensor_path)) << "previous boot data\n";

    // Both a previous directory and an unrelated file must cause retries.
    std::ofstream(root + "/00000000000000000000000000000001") << "unrelated file\n";
    next_id = 0;
    storage::LogSession reboot;
    assert(reboot.create(random_bytes, make_directory) == 0);
    assert(std::string(reboot.directory) == "/SD:/00000000000000000000000000000002");
    std::string content;
    std::ifstream old_log(local_path(first.sensor_path));
    std::getline(old_log, content);
    assert(content == "previous boot data");

    // A file-setup retry keeps the directory it has already reserved.
    const unsigned calls = mkdir_calls;
    assert(first.create(random_bytes, make_directory) == 0);
    assert(mkdir_calls == calls);

    for (int error : {-EIO, -ENOSPC, -EROFS}) {
        storage::LogSession failed;
        mkdir_error = error;
        assert(failed.create(random_bytes, make_directory) == error);
        assert(failed.directory[0] == '\0');
        assert(failed.sensor_path[0] == '\0');
        assert(failed.system_path[0] == '\0');
        mkdir_error = 0;
        assert(failed.create(random_bytes, make_directory) == 0);
    }

    storage::LogSession no_entropy;
    random_error = -ENODEV;
    assert(no_entropy.create(random_bytes, make_directory) == -ENODEV);
    assert(no_entropy.directory[0] == '\0');
    random_error = 0;
    assert(no_entropy.create(random_bytes, make_directory) == 0);

    // A faulty repeating RNG must fail, never append to an old boot's logs.
    storage::LogSession stuck;
    next_id = 0;
    repeat_id = true;
    assert(stuck.create(random_bytes, make_directory) == -EEXIST);
    assert(stuck.directory[0] == '\0');
    std::filesystem::remove_all(root);
}
