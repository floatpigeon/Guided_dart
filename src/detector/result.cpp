#ifndef DART_SRC_DETECTOR_RESULT_CPP
#define DART_SRC_DETECTOR_RESULT_CPP

#include <chrono>
#include <cstdint>

#include "geometry.cpp"

namespace dart::detection {

class Target {
public:
    bool valid = false;
    Point center{};
    Rect bounds{};
    std::uint32_t area = 0;
    float radius = 0.0F;
    float quality = 0.0F;
    bool clipped = false;
};

class FrameMetadata {
public:
    std::uint64_t frame_id = 0;
    std::uint64_t source_pts = 0;
    std::chrono::microseconds received{};
    bool source_pts_timebase_validated = false;
};

class ScanMetadata {
public:
    Rect search_window{};
    bool full_frame = false;
    bool rescanned = false;
    std::uint32_t candidates = 0;
};

enum class TargetKind : std::uint8_t { kNone, kLight, kArmor };

class DetectResult {
public:
    TargetKind kind = TargetKind::kNone;
    Target target{}; // Armor center when available, otherwise light center.
    Target light{};
    Target armor{};
    FrameMetadata source{};
    ScanMetadata scan{};
    std::uint32_t missed_frames = 0;
};

} // namespace dart::detection

#endif // DART_SRC_DETECTOR_RESULT_CPP
