#ifndef DART_APP_LOGGER_CPP
#define DART_APP_LOGGER_CPP

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>

#include "monotonic_clock.cpp"

namespace dart::app {

class Logger {
public:
    bool open(std::string_view path) noexcept {
        if (path.empty() || path.size() >= sizeof(path_))
            return false;
        std::memcpy(path_, path.data(), path.size());
        path_[path.size()] = '\0';
        const int fd = ::open(path_, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0)
            return false;
        ::close(fd);
        return true;
    }

    void begin_batch() noexcept { batching_ = true; }

    void end_batch() noexcept {
        batching_ = false;
        if (fd_ >= 0) {
            ::fsync(fd_);
            ::close(fd_);
            fd_ = -1;
        }
    }

    void line(const char* fmt, ...) noexcept {
        char body[512];
        char out[640];
        va_list ap;
        va_start(ap, fmt);
        const int body_n = std::vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);
        if (body_n < 0)
            return;
        int out_n = std::snprintf(
            out, sizeof(out), "ts_ms=%lld %s", static_cast<long long>(mono_ms()),
            body);
        if (out_n < 0)
            return;
        if (static_cast<std::size_t>(out_n) >= sizeof(out))
            out_n = static_cast<int>(sizeof(out)) - 1;
        if (fd_ < 0 && path_[0] != '\0')
            fd_ = ::open(path_, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd_ >= 0) {
            (void)!::write(fd_, out, static_cast<std::size_t>(out_n));
            if (!batching_)
                end_batch();
        } else {
            (void)!::write(STDERR_FILENO, out, static_cast<std::size_t>(out_n));
        }
    }

private:
    char path_[512]{};
    int fd_ = -1;
    bool batching_ = false;
};

} // namespace dart::app

#endif // DART_APP_LOGGER_CPP
