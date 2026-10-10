#ifndef DART_CAMERA_EXPOSURE_CONTROL_HPP
#define DART_CAMERA_EXPOSURE_CONTROL_HPP

#include <cstdint>
#include <expected>
#include <type_traits>
#include "core/status.h"

namespace dart::camera {

using Error = dart_error_t;
static_assert(std::is_trivially_copyable_v<Error>);

struct ExposureResult {
    std::uint32_t requested_us;
    std::uint32_t applied_us; // Accepted command; sensor line quantization may differ.
    float min_us;
    float max_us;
    bool clamped;
};

class IExposureControl {
public:
    virtual ~IExposureControl() = default;
    virtual std::expected<ExposureResult, Error>
    set_exposure_us(std::uint32_t exposure_us) noexcept = 0;
};

inline std::expected<ExposureResult, Error>
set_fixed_exposure(IExposureControl &control, std::uint32_t exposure_us) noexcept {
    return control.set_exposure_us(exposure_us);
}

} // namespace dart::camera
#endif
