#ifndef DART_INCLUDE_FIRMWARE_APP_SRC_CAMERA_INTERFACE_CPP
#define DART_INCLUDE_FIRMWARE_APP_SRC_CAMERA_INTERFACE_CPP

#include <chrono>
#include <cstdint>
#include <expected>

#include "frame_lease.cpp"

namespace dart::camera {

enum class ExposureMode : std::uint32_t {
    kFixed = 0,
    kAuto = 1,
};

class Config {
public:
    std::uint32_t csi_port = 0;
    std::uint32_t sensor_width = 0;
    std::uint32_t sensor_height = 0;
    std::uint32_t sensor_fps = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t capture_buffers = 4;
    std::uint32_t channel_buffers = 4;
    ExposureMode exposure_mode = ExposureMode::kFixed;
    std::uint32_t initial_exposure_us = 0;
    std::uint32_t mirror = 0;
};

class Caps {
public:
    std::uint32_t sensor_width = 0;
    std::uint32_t sensor_height = 0;
    std::uint32_t sensor_fps = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    ExposureMode exposure_mode = ExposureMode::kFixed;
};

enum class AcquireStatus : std::uint8_t {
    /* The lease holds a frame, valid until it is released. */
    kFrame,
    /* No frame arrived inside the timeout: a normal negative result, not an
     * error, and it allocates nothing.
     */
    kNoFrame,
    /* Every lease slot is held, so no frame was taken from the driver and the
     * frame is recycled there: the bounded-capacity drop signal.
     */
    kLeaseExhausted,
};

class AcquireResult {
public:
    AcquireStatus status = AcquireStatus::kNoFrame;
    FrameLease lease{};
};

enum class State : std::uint32_t {
    kEmpty = 0,
    kInitialized = 1,
    kStarted = 2,
    kFaulted = 3,
};

class ICamera {
public:
    virtual ~ICamera() = default;

    virtual std::expected<void, CameraError> start() noexcept = 0;
    /* Three outcomes, only one of which is an error: a frame, no frame, or a full
     * lease pool. A caller with its own bounded pipeline drops on
     * kLeaseExhausted and counts it.
     */
    virtual std::expected<AcquireResult, CameraError>
        acquire(std::chrono::milliseconds timeout) noexcept = 0;
    virtual std::expected<void, CameraError> stop() noexcept = 0;
    virtual State state() const noexcept = 0;
    virtual const Caps& caps() const noexcept = 0;
    virtual std::expected<void, CameraError> set_fixed_exposure_us(std::uint32_t us) noexcept = 0;

    /* Lease slots fixed at creation, and how many are in use right now. A
     * producer can check these before acquiring to apply its own drop policy.
     */
    virtual std::uint32_t capacity() const noexcept = 0;
    virtual std::uint32_t outstanding() const noexcept = 0;
};

} // namespace dart::camera

#endif // DART_INCLUDE_FIRMWARE_APP_SRC_CAMERA_INTERFACE_CPP
