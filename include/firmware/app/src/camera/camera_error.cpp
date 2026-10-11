#include "camera.hpp"
#include "camera_error.hpp"

#include <array>
#include <charconv>
#include <cstring>
#include <new>
#include <cmath>
#include <utility>

namespace dart::camera {
namespace {

constexpr std::array<std::int32_t, 25> kSiteCodes = {
    DART_CAMERA_E_INVALID_ARGUMENT,
    DART_CAMERA_E_INVALID_STATE,
    DART_CAMERA_E_BUSY,
    DART_CAMERA_E_NOT_SUPPORTED,
    DART_CAMERA_E_OUT_OF_RANGE,
    DART_CAMERA_E_STALE_LEASE,
    DART_CAMERA_E_NO_MEMORY,
    DART_CAMERA_E_NO_FRAME,
    DART_CAMERA_E_CONFIG_VERSION,
    DART_CAMERA_E_CONFIG_INVALID,
    DART_CAMERA_E_SENSOR_PROBE_FAILED,
    DART_CAMERA_E_SENSOR_MODE_MISMATCH,
    DART_CAMERA_E_POOL_CREATE_FAILED,
    DART_CAMERA_E_BLOCK_EXHAUSTED,
    DART_CAMERA_E_MAP_FAILED,
    DART_CAMERA_E_BLOCK_RELEASE_FAILED,
    DART_CAMERA_E_FRAME_GEOMETRY_MISMATCH,
    DART_CAMERA_E_FRAME_POOL_MISMATCH,
    DART_CAMERA_E_FRAME_NOT_MAPPED,
    DART_CAMERA_E_CACHE_INVALIDATE_FAILED,
    DART_CAMERA_E_CLOCK_FAILED,
    DART_CAMERA_E_EXPOSURE_RANGE_FAILED,
    DART_CAMERA_E_EXPOSURE_WRITE_FAILED,
    DART_CAMERA_E_LEASE_ABI_MISMATCH,
    DART_CAMERA_E_SITE_CODE_MAX,
};

consteval bool site_codes_are_unique() {
    for (std::size_t i = 0; i < kSiteCodes.size(); ++i) {
        for (std::size_t j = i + 1; j < kSiteCodes.size(); ++j) {
            if (kSiteCodes[i] == kSiteCodes[j]) return false;
        }
    }
    return true;
}

static_assert(DART_CAMERA_E_SITE_CODE_MAX < DART_CAMERA_CODE_ERRNO_MAX,
              "site code range must remain below errno range");
static_assert(DART_CAMERA_CODE_VENDOR_MAX < -DART_CAMERA_CODE_ERRNO_MAX,
              "vendor code range must remain below errno range");
static_assert(site_codes_are_unique(), "camera site codes must be unique");

template <typename Error>
ErrorPtr make_error(std::int32_t code, const CallContext& context) noexcept {
    return ErrorPtr(new (std::nothrow) Error(code, context));
}

}  // namespace

ErrorTextSink::ErrorTextSink(char* buffer, std::size_t capacity) noexcept
    : buffer_(buffer), capacity_(capacity), size_(0), truncated_(false), has_field_(false) {
    if (buffer_ != nullptr && capacity_ != 0) buffer_[0] = '\0';
}

void ErrorTextSink::append(std::string_view text) noexcept {
    if (text.empty()) return;
    if (buffer_ == nullptr || capacity_ == 0 || size_ >= capacity_) {
        truncated_ = true;
        return;
    }
    const std::size_t available = capacity_ - 1U - size_;
    const std::size_t count = text.size() < available ? text.size() : available;
    if (count != 0) {
        std::memcpy(buffer_ + size_, text.data(), count);
        size_ += count;
        buffer_[size_] = '\0';
    }
    if (count != text.size()) truncated_ = true;
}

void ErrorTextSink::begin_field(std::string_view key) noexcept {
    if (has_field_) append(" ");
    has_field_ = true;
    append(key);
    append("=");
}

void ErrorTextSink::append_field(std::string_view key, std::string_view value) noexcept {
    begin_field(key);
    append(value);
}

void ErrorTextSink::append_unsigned(std::uint64_t value) noexcept {
    char text[24] = {};
    const auto result = std::to_chars(text, text + sizeof(text), value);
    if (result.ec == std::errc{}) append(std::string_view(text, static_cast<std::size_t>(result.ptr - text)));
    else append("?");
}

void ErrorTextSink::append_signed(std::int64_t value) noexcept {
    char text[24] = {};
    const auto result = std::to_chars(text, text + sizeof(text), value);
    if (result.ec == std::errc{}) append(std::string_view(text, static_cast<std::size_t>(result.ptr - text)));
    else append("?");
}

void ErrorTextSink::append_field(std::string_view key, std::uint64_t value) noexcept {
    begin_field(key);
    append_unsigned(value);
}

void ErrorTextSink::append_field(std::string_view key, std::int64_t value) noexcept {
    begin_field(key);
    append_signed(value);
}

void ErrorTextSink::append_field(std::string_view key, std::uint32_t value) noexcept {
    append_field(key, static_cast<std::uint64_t>(value));
}

void ErrorTextSink::append_field(std::string_view key, std::int32_t value) noexcept {
    append_field(key, static_cast<std::int64_t>(value));
}

void ErrorTextSink::append_field(std::string_view key, bool value) noexcept {
    begin_field(key);
    append(value ? "true" : "false");
}

void ErrorTextSink::append_hex_field(std::string_view key, std::uint64_t value) noexcept {
    begin_field(key);
    char text[18] = {};
    const auto result = std::to_chars(text, text + sizeof(text), value, 16);
    if (result.ec == std::errc{}) append(std::string_view(text, static_cast<std::size_t>(result.ptr - text)));
    else append("?");
}

void ErrorTextSink::append_float_field(std::string_view key, float value) noexcept {
    begin_field(key);
    char text[32] = {};
    const auto result = std::to_chars(text, text + sizeof(text), value,
                                      std::chars_format::fixed, 3);
    if (result.ec == std::errc{}) append(std::string_view(text, static_cast<std::size_t>(result.ptr - text)));
    else append("?");
}

void CameraError::append_context(ErrorTextSink& sink) const noexcept {
    sink.append_field("code", static_cast<std::int64_t>(code_));
    sink.append_field("op", operation_name(context_.operation));
    if (context_.native_hint != 0) {
        sink.append_field("native_hint", static_cast<std::int64_t>(context_.native_hint));
    }
    if (context_.requested != 0) sink.append_field("requested", context_.requested);
    if (context_.observed != 0) sink.append_field("observed", context_.observed);
    if (context_.state != 0 || context_.required_state != 0) {
        sink.append_field("state", context_.state);
        sink.append_field("required_state", context_.required_state);
    }
    if (context_.lease_token != 0) sink.append_field("lease_token", context_.lease_token);
    if (context_.lease_generation != 0) sink.append_field("lease_generation", context_.lease_generation);
    if (context_.lease_owner != 0) sink.append_hex_field("lease_owner", context_.lease_owner);
    if (context_.rollback_failed) sink.append_field("rollback_failed", true);
    if (context_.previous_release_code != 0) sink.append_field("previous_release_code", static_cast<std::int64_t>(context_.previous_release_code));
}

std::uint32_t HardwareError::vendor_module() const noexcept {
    return (static_cast<std::uint32_t>(code_) >> 16U) & 0xffU;
}

std::uint32_t HardwareError::vendor_level() const noexcept {
    return (static_cast<std::uint32_t>(code_) >> 13U) & 0x7U;
}

std::uint32_t HardwareError::vendor_error_id() const noexcept {
    return static_cast<std::uint32_t>(code_) & 0x1fffU;
}

void HardwareError::append_context(ErrorTextSink& sink) const noexcept {
    CameraError::append_context(sink);
    if (code_ <= DART_CAMERA_CODE_VENDOR_MAX) {
        sink.append_field("module", vendor_module());
        sink.append_field("level", vendor_level());
        sink.append_field("errid", vendor_error_id());
    }
}

void SensorModeMismatchError::append_context(ErrorTextSink& sink) const noexcept {
    CameraError::append_context(sink);
    if (!context_.has_sensor_mode) return;
    const auto& requested = context_.requested_sensor;
    const auto& observed = context_.observed_sensor;
    sink.append_field("req_csi", requested.csi_port);
    sink.append_field("req_w", requested.width);
    sink.append_field("req_h", requested.height);
    sink.append_field("req_fps", requested.fps);
    sink.append_field("obs_csi", observed.csi_port);
    sink.append_field("obs_w", observed.width);
    sink.append_field("obs_h", observed.height);
    sink.append_field("obs_fps", observed.fps);
}

void FrameGeometryError::append_context(ErrorTextSink& sink) const noexcept {
    CameraError::append_context(sink);
    if (context_.has_frame_geometry) {
        const auto& frame = context_.frame_geometry;
        sink.append_field("width", frame.width);
        sink.append_field("height", frame.height);
        sink.append_field("stride", frame.stride);
        sink.append_field("format", frame.format);
        sink.append_field("pool_id", frame.pool_id);
        sink.append_hex_field("phys", frame.physical_address);
        sink.append_field("length", frame.length);
        sink.append_field("block_bytes", frame.block_bytes);
    }
}

void CacheSyncError::append_context(ErrorTextSink& sink) const noexcept {
    CameraError::append_context(sink);
    sink.append_hex_field("phys", context_.physical_address);
    sink.append_hex_field("mapped", static_cast<std::uint64_t>(context_.mapped_address));
    sink.append_field("length", context_.length);
}

void PlatformError::append_context(ErrorTextSink& sink) const noexcept {
    CameraError::append_context(sink);
    if (code_ < 0) sink.append_field("errno", static_cast<std::int64_t>(-code_));
}

void ExposureRangeError::append_context(ErrorTextSink& sink) const noexcept {
    CameraError::append_context(sink);
    if (context_.has_exposure) {
        sink.append_float_field("requested_us", context_.requested_us);
        sink.append_float_field("min_us", context_.min_us);
        sink.append_float_field("max_us", context_.max_us);
        sink.append_float_field("applied_us", context_.applied_us);
    }
}

std::string_view operation_name(Operation operation) noexcept {
    switch (operation) {
        case Operation::kConfigValidate: return "config_validate";
        case Operation::kMediaCreate: return "media_create";
        case Operation::kMediaDestroy: return "media_destroy";
        case Operation::kCreate: return "create";
        case Operation::kInitialize: return "init";
        case Operation::kStart: return "start";
        case Operation::kAcquire: return "acquire";
        case Operation::kRelease: return "release";
        case Operation::kStop: return "stop";
        case Operation::kDeinitialize: return "deinit";
        case Operation::kDestroy: return "destroy";
        case Operation::kGetState: return "get_state";
        case Operation::kGetCaps: return "get_caps";
        case Operation::kExposureRange: return "exposure_range";
        case Operation::kSetExposure: return "set_exposure";
        case Operation::kUnknown: break;
    }
    return "unknown";
}

ErrorPtr translate_camera_code(std::int32_t code, const CallContext& context) noexcept {
    if (code == DART_CAMERA_OK || code == DART_CAMERA_E_NO_FRAME) return nullptr;

    if (code < 0 && code >= -DART_CAMERA_CODE_ERRNO_MAX) {
        return make_error<PlatformError>(code, context);
    }
    if (code <= DART_CAMERA_CODE_VENDOR_MAX) {
        return make_error<HardwareError>(code, context);
    }

    switch (code) {
        case DART_CAMERA_E_INVALID_ARGUMENT:
        case DART_CAMERA_E_CONFIG_VERSION:
        case DART_CAMERA_E_CONFIG_INVALID:
        case DART_CAMERA_E_LEASE_ABI_MISMATCH:
            return make_error<ArgumentError>(code, context);
        case DART_CAMERA_E_INVALID_STATE:
            return make_error<StateError>(code, context);
        case DART_CAMERA_E_BUSY:
            return make_error<BusyError>(code, context);
        case DART_CAMERA_E_NOT_SUPPORTED:
            return make_error<UnsupportedError>(code, context);
        case DART_CAMERA_E_OUT_OF_RANGE:
            return make_error<OutOfRangeError>(code, context);
        case DART_CAMERA_E_NO_MEMORY:
            return make_error<NoMemoryError>(code, context);
        case DART_CAMERA_E_STALE_LEASE:
            return make_error<StaleLeaseError>(code, context);
        case DART_CAMERA_E_SENSOR_MODE_MISMATCH:
            return make_error<SensorModeMismatchError>(code, context);
        case DART_CAMERA_E_FRAME_GEOMETRY_MISMATCH:
        case DART_CAMERA_E_FRAME_POOL_MISMATCH:
        case DART_CAMERA_E_FRAME_NOT_MAPPED:
            return make_error<FrameGeometryError>(code, context);
        case DART_CAMERA_E_CACHE_INVALIDATE_FAILED:
            return make_error<CacheSyncError>(code, context);
        case DART_CAMERA_E_BLOCK_RELEASE_FAILED:
            return make_error<ReleaseError>(code, context);
        case DART_CAMERA_E_EXPOSURE_RANGE_FAILED:
        case DART_CAMERA_E_EXPOSURE_WRITE_FAILED:
            return make_error<ExposureRangeError>(code, context);
        case DART_CAMERA_E_CLOCK_FAILED:
            return make_error<PlatformError>(code, context);
        case DART_CAMERA_E_SENSOR_PROBE_FAILED:
        case DART_CAMERA_E_POOL_CREATE_FAILED:
        case DART_CAMERA_E_BLOCK_EXHAUSTED:
        case DART_CAMERA_E_MAP_FAILED:
        default:
            return make_error<HardwareError>(code, context);
    }
}

FrameLease::FrameLease(FrameLease&& other) noexcept
    : state_(std::move(other.state_)) {}

FrameLease& FrameLease::operator=(FrameLease&& other) noexcept {
    if (this == &other) return *this;
    if (state_ != nullptr && !state_->released) {
        auto result = release();
        if (!result && state_ != nullptr && state_->remember_failure != nullptr) {
            state_->remember_failure(state_.get(), std::move(result.error()));
        }
    }
    state_ = std::move(other.state_);
    return *this;
}

FrameLease::~FrameLease() {
    if (state_ == nullptr || state_->released) return;
    auto result = release();
    if (!result && state_ != nullptr && state_->remember_failure != nullptr) {
        state_->remember_failure(state_.get(), std::move(result.error()));
    }
}

const FrameView& FrameLease::view() const noexcept {
    static const FrameView empty{};
    if (state_ == nullptr || state_->released) return empty;
    return state_->view;
}

std::expected<void, ErrorPtr> FrameLease::release() noexcept {
    if (state_ == nullptr || state_->released) return {};
    if (state_->release == nullptr) {
        state_->released = true;
        return {};
    }
    auto result = state_->release(state_.get());
    if (result) {
        state_->released = true;
        state_.reset();
    }
    return result;
}

std::expected<ExposurePlan, ErrorPtr> plan_fixed_exposure_us(
    std::uint32_t requested_us, float min_us, float max_us,
    Operation operation) noexcept {
    CallContext context{};
    context.operation = operation;
    context.has_exposure = true;
    context.requested_us = static_cast<float>(requested_us);
    context.min_us = min_us;
    context.max_us = max_us;
    const float lower = std::ceil(min_us);
    const float upper = std::floor(max_us);
    if (!std::isfinite(min_us) || !std::isfinite(max_us) || !std::isfinite(lower) ||
        !std::isfinite(upper) || min_us < 0.0F || max_us < min_us ||
        upper < lower || upper > 4294967295.0F) {
        return std::unexpected<ErrorPtr>(
            ErrorPtr(new (std::nothrow) ExposureRangeError(
                DART_CAMERA_E_OUT_OF_RANGE, context)));
    }
    const auto first = static_cast<std::uint32_t>(lower);
    const auto last = static_cast<std::uint32_t>(upper);
    const auto applied = requested_us < first ? first : (requested_us > last ? last : requested_us);
    context.applied_us = static_cast<float>(applied);
    ExposurePlan plan{};
    plan.requested_us = requested_us;
    plan.applied_us = applied;
    plan.min_us = min_us;
    plan.max_us = max_us;
    plan.seconds = static_cast<float>(applied) / 1000000.0F;
    return plan;
}

}  // namespace dart::camera
