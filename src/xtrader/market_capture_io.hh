// Checked POSIX I/O shared by the standalone market worker and config utility.
// Source: user request, 2026-11-13. Signature: GitHub Copilot (Auto).
#pragma once

#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace seastar::xtrader::capture_detail {

[[noreturn]] inline void io_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

class file_descriptor {
public:
    explicit file_descriptor(int value = -1) noexcept : _value(value) {}
    ~file_descriptor() { if (_value >= 0) { ::close(_value); } }
    file_descriptor(const file_descriptor&) = delete;
    file_descriptor& operator=(const file_descriptor&) = delete;
    file_descriptor(file_descriptor&& other) noexcept : _value(std::exchange(other._value, -1)) {}
    file_descriptor& operator=(file_descriptor&& other) noexcept {
        if (this != &other) {
            if (_value >= 0) { ::close(_value); }
            _value = std::exchange(other._value, -1);
        }
        return *this;
    }
    int get() const noexcept { return _value; }
private:
    int _value;
};

inline void write_all(int fd, const char* data, std::size_t size) {
    while (size) {
        const auto written = ::write(fd, data, size);
        if (written < 0 && errno == EINTR) { continue; }
        if (written <= 0) {
            if (written == 0) { errno = EIO; }
            io_error("write market file");
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

inline void sync_file(int fd) {
    while (::fsync(fd) != 0) {
        if (errno != EINTR) { io_error("sync market file"); }
    }
}

inline file_descriptor open_private_file(const std::filesystem::path& path) {
    file_descriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0) { io_error("open private configuration/key file"); }
    struct stat status{};
    if (::fstat(file.get(), &status) != 0) { io_error("inspect private file"); }
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid()
            || (status.st_mode & 077) != 0 || status.st_nlink != 1) {
        throw std::runtime_error("configuration/key must be a regular, single-link, owner-only file owned by this user");
    }
    return file;
}

inline std::string read_private_file(const std::filesystem::path& path, std::size_t limit) {
    auto file = open_private_file(path);
    std::string result;
    char buffer[4096];
    while (true) {
        const auto count = ::read(file.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) { continue; }
        if (count < 0) { io_error("read private file"); }
        if (count == 0) { return result; }
        if (result.size() + static_cast<std::size_t>(count) > limit) {
            throw std::runtime_error("configuration/key file exceeds its size limit");
        }
        result.append(buffer, static_cast<std::size_t>(count));
    }
}

inline void create_private_file(const std::filesystem::path& path, const std::string& data) {
    file_descriptor file(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.get() < 0) { io_error("create private file (existing files are never overwritten)"); }
    try {
        write_all(file.get(), data.data(), data.size());
        sync_file(file.get());
    } catch (...) {
        if (::unlink(path.c_str()) != 0) {
            std::throw_with_nested(std::runtime_error("write failed and incomplete private file could not be removed"));
        }
        throw;
    }
}

} // namespace seastar::xtrader::capture_detail
