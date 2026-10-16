#ifndef DART_SRC_DETECTOR_GEOMETRY_CPP
#define DART_SRC_DETECTOR_GEOMETRY_CPP

#include <algorithm>
#include <cstdint>

namespace dart::detection {

struct Point {
    float x = 0.0F;
    float y = 0.0F;
};

struct Rect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

inline Rect FullWindow(std::uint32_t width, std::uint32_t height) noexcept {
    return {0, 0, width, height};
}

inline bool IsFullWindow(Rect window, std::uint32_t width, std::uint32_t height) noexcept {
    return window.x == 0 && window.y == 0 && window.width == width && window.height == height;
}

inline Rect UniteWindows(Rect a, Rect b) noexcept {
    if (b.width == 0 || b.height == 0)
        return a;
    const auto x = std::min(a.x, b.x);
    const auto y = std::min(a.y, b.y);
    return {
        x, y, std::max(a.x + a.width, b.x + b.width) - x,
        std::max(a.y + a.height, b.y + b.height) - y};
}

} // namespace dart::detection

#endif // DART_SRC_DETECTOR_GEOMETRY_CPP
