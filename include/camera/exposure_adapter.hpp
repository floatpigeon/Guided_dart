#ifndef DART_CAMERA_EXPOSURE_ADAPTER_HPP
#define DART_CAMERA_EXPOSURE_ADAPTER_HPP

#include "camera.h"
#include "camera/exposure_control.hpp"

namespace dart::camera {

// Composition root constructs the adapter. Upper layers receive the interface.
// Camera must outlive this non-owning object; calls must be serialized.
class CameraExposureControl final : public IExposureControl {
public:
    explicit CameraExposureControl(dart_camera_t &camera) noexcept : camera_(camera) {}
    CameraExposureControl(const CameraExposureControl &) = delete;
    CameraExposureControl &operator=(const CameraExposureControl &) = delete;

    std::expected<ExposureResult, Error>
    set_exposure_us(std::uint32_t exposure_us) noexcept override {
        dart_exposure_result_t result{};
        const auto status = dart_camera_set_fixed_exposure_us(&camera_, exposure_us, &result);
        if (status != DART_STATUS_OK) {
            Error error{1, sizeof(Error), status, DART_OP_EXPOSURE_WRITE, 0, 0};
            (void)dart_camera_get_error(&camera_, &error);
            error.status = status;
            return std::unexpected(error);
        }
        return ExposureResult{result.requested_us, result.applied_us,
                              result.min_us, result.max_us, result.clamped != 0};
    }

private:
    dart_camera_t &camera_;
};

} // namespace dart::camera
#endif
