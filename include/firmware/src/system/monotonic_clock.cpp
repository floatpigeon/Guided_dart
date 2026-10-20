#ifndef DART_SYSTEM_MONOTONIC_CLOCK_CPP
#define DART_SYSTEM_MONOTONIC_CLOCK_CPP

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <time.h>

namespace dart::platform {

inline std::int64_t mono_ms() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

inline void sleep_ms(int ms) noexcept {
    if (ms <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

} // namespace dart::platform

#endif // DART_SYSTEM_MONOTONIC_CLOCK_CPP
