#ifndef DART_CAMERA_ERROR_HPP
#define DART_CAMERA_ERROR_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "camera_error_codes.h"

namespace dart::camera {

enum class ErrorKind {
    kArgument,
    kState,
    kBusy,
    kUnsupported,
    kOutOfRange,
    kNoMemory,
    kStaleLease,
    kHardware,
    kCacheSync,
    kRelease,
    kPlatform,
    kExposure,
};

enum class Operation {
    kUnknown,
    kConfigValidate,
    kMediaCreate,
    kMediaDestroy,
    kCreate,
    kInitialize,
    kStart,
    kAcquire,
    kRelease,
    kStop,
    kDeinitialize,
    kDestroy,
    kGetState,
    kGetCaps,
    kExposureRange,
    kSetExposure,
};

struct SensorModeContext {
    std::uint32_t csi_port = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps = 0;
    std::uint32_t hdr_mode = 0;
};

struct FrameGeometryContext {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = 0;
    std::uint32_t pool_id = 0;
    std::uint64_t physical_address = 0;
    std::uint64_t length = 0;
    std::uint64_t block_bytes = 0;
};

struct CallContext {
    Operation operation = Operation::kUnknown;
    std::int32_t native_hint = 0;
    std::uint32_t requested = 0;
    std::uint32_t observed = 0;
    std::uint32_t state = 0;
    std::uint32_t required_state = 0;
    bool rollback_failed = false;
    bool has_sensor_mode = false;
    SensorModeContext requested_sensor{};
    SensorModeContext observed_sensor{};
    bool has_frame_geometry = false;
    FrameGeometryContext frame_geometry{};
    std::uint64_t physical_address = 0;
    std::uintptr_t mapped_address = 0;
    std::uint64_t length = 0;
    std::uint64_t block_bytes = 0;
    bool has_exposure = false;
    float requested_us = 0.0F;
    float min_us = 0.0F;
    float max_us = 0.0F;
    float applied_us = 0.0F;
    std::int32_t previous_release_code = 0;
    std::uint64_t lease_token = 0;
    std::uint64_t lease_generation = 0;
    std::uintptr_t lease_owner = 0;
};

class ErrorTextSink {
public:
    ErrorTextSink(char* buffer, std::size_t capacity) noexcept;

    void append(std::string_view text) noexcept;
    void append_field(std::string_view key, std::string_view value) noexcept;
    void append_field(std::string_view key, std::uint64_t value) noexcept;
    void append_field(std::string_view key, std::int64_t value) noexcept;
    void append_field(std::string_view key, std::uint32_t value) noexcept;
    void append_field(std::string_view key, std::int32_t value) noexcept;
    void append_field(std::string_view key, bool value) noexcept;
    void append_hex_field(std::string_view key, std::uint64_t value) noexcept;
    void append_float_field(std::string_view key, float value) noexcept;

    std::size_t size() const noexcept { return size_; }
    bool truncated() const noexcept { return truncated_; }

private:
    void begin_field(std::string_view key) noexcept;
    void append_unsigned(std::uint64_t value) noexcept;
    void append_signed(std::int64_t value) noexcept;

    char* buffer_;
    std::size_t capacity_;
    std::size_t size_;
    bool truncated_;
    bool has_field_;
};

class CameraError {
public:
    virtual ~CameraError() = default;
    virtual ErrorKind kind() const noexcept = 0;
    virtual std::string_view category() const noexcept = 0;
    virtual std::int32_t code() const noexcept { return code_; }
    virtual Operation operation() const noexcept { return context_.operation; }
    virtual std::string_view message() const noexcept = 0;
    virtual void append_context(ErrorTextSink& sink) const noexcept;

    const CallContext& context() const noexcept { return context_; }

    CameraError(std::int32_t code, const CallContext& context) noexcept
        : code_(code), context_(context) {}

protected:
    std::int32_t code_;
    CallContext context_;
};

using ErrorPtr = std::unique_ptr<const CameraError>;

class ArgumentError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kArgument; }
    std::string_view category() const noexcept override { return "camera.argument"; }
    std::string_view message() const noexcept override { return "invalid camera argument or configuration"; }
};

class StateError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kState; }
    std::string_view category() const noexcept override { return "camera.state"; }
    std::string_view message() const noexcept override { return "camera operation is invalid in the current state"; }
};

class BusyError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kBusy; }
    std::string_view category() const noexcept override { return "camera.busy"; }
    std::string_view message() const noexcept override { return "camera resource is still owned"; }
};

class UnsupportedError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kUnsupported; }
    std::string_view category() const noexcept override { return "camera.unsupported"; }
    std::string_view message() const noexcept override { return "camera operation is not supported"; }
};

class OutOfRangeError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kOutOfRange; }
    std::string_view category() const noexcept override { return "camera.range"; }
    std::string_view message() const noexcept override { return "camera value is outside the supported range"; }
};

class NoMemoryError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kNoMemory; }
    std::string_view category() const noexcept override { return "camera.memory"; }
    std::string_view message() const noexcept override { return "camera allocation failed"; }
};

class StaleLeaseError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kStaleLease; }
    std::string_view category() const noexcept override { return "camera.lease"; }
    std::string_view message() const noexcept override { return "frame lease is stale or belongs to another camera"; }
};

class HardwareError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kHardware; }
    std::string_view category() const noexcept override { return "camera.hardware"; }
    std::string_view message() const noexcept override { return "camera hardware operation failed"; }
    void append_context(ErrorTextSink& sink) const noexcept override;

    std::uint32_t vendor_module() const noexcept;
    std::uint32_t vendor_level() const noexcept;
    std::uint32_t vendor_error_id() const noexcept;
};

class SensorModeMismatchError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kHardware; }
    std::string_view category() const noexcept override { return "camera.sensor_mode"; }
    std::string_view message() const noexcept override { return "sensor did not provide the exact requested mode"; }
    void append_context(ErrorTextSink& sink) const noexcept override;
};

class FrameGeometryError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kHardware; }
    std::string_view category() const noexcept override { return "camera.frame_geometry"; }
    std::string_view message() const noexcept override { return "frame geometry or pool ownership did not match"; }
    void append_context(ErrorTextSink& sink) const noexcept override;
};

class CacheSyncError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kCacheSync; }
    std::string_view category() const noexcept override { return "camera.cache"; }
    std::string_view message() const noexcept override { return "frame cache invalidation failed"; }
    void append_context(ErrorTextSink& sink) const noexcept override;
};

class ReleaseError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kRelease; }
    std::string_view category() const noexcept override { return "camera.release"; }
    std::string_view message() const noexcept override { return "camera resource release failed"; }
};

class PlatformError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kPlatform; }
    std::string_view category() const noexcept override { return "camera.platform"; }
    std::string_view message() const noexcept override { return "platform operation failed"; }
    void append_context(ErrorTextSink& sink) const noexcept override;
};

class ExposureRangeError final : public CameraError {
public:
    using CameraError::CameraError;
    ErrorKind kind() const noexcept override { return ErrorKind::kExposure; }
    std::string_view category() const noexcept override { return "camera.exposure"; }
    std::string_view message() const noexcept override { return "exposure operation failed"; }
    void append_context(ErrorTextSink& sink) const noexcept override;
};

using ExposureError = ExposureRangeError;

/* The sole translation point from C ABI integers into polymorphic errors.
 * Success (0) and DART_CAMERA_E_NO_FRAME intentionally return an empty ptr.
 * If allocating a diagnostic fails, the error pointer is empty as well; the
 * expected still contains an error alternative, allowing callers to detect
 * the allocation failure without exceptions.
 */
ErrorPtr translate_camera_code(std::int32_t code, const CallContext& context) noexcept;
std::string_view operation_name(Operation operation) noexcept;

}  // namespace dart::camera

#endif
