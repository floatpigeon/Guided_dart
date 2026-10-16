#ifndef DART_SRC_DETECTOR_INTERFACE_CPP
#define DART_SRC_DETECTOR_INTERFACE_CPP

#include <expected>

#include "camera/frame_view.cpp"
#include "result.cpp"

namespace dart::detection {

enum class DetectionError : int {
    kInvalidConfig = 1,
    kNoMemory,
    kNotPrepared,
    kInvalidFrame,
    kInvalidWindow,
    kWorkspaceExhausted,
    kOutOfOrderFrame,
};

class IDetector {
public:
    virtual ~IDetector() = default;
    virtual std::expected<DetectResult, DetectionError>
        Detect(const camera::FrameView& frame) noexcept = 0;
    virtual void Reset() noexcept = 0;
};

} // namespace dart::detection

#endif // DART_SRC_DETECTOR_INTERFACE_CPP
