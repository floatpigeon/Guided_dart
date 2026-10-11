#ifndef DART_CAMERA_HPP
#define DART_CAMERA_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>

#include "camera_error.hpp"

namespace dart::camera {

enum class State : std::uint32_t {
    kEmpty = 0,
    kInitialized = 1,
    kStarted = 2,
    kFaulted = 3,
};

enum class ExposureMode : std::uint32_t {
    kFixed = 0,
    kAuto = 1,
};

enum class FrameFlag : std::uint32_t {
    kSourcePtsUnvalidated = 1U,
};

static_assert(static_cast<std::uint32_t>(State::kEmpty) == 0U);
static_assert(static_cast<std::uint32_t>(State::kInitialized) == 1U);
static_assert(static_cast<std::uint32_t>(State::kStarted) == 2U);
static_assert(static_cast<std::uint32_t>(State::kFaulted) == 3U);
static_assert(static_cast<std::uint32_t>(ExposureMode::kFixed) == 0U);
static_assert(static_cast<std::uint32_t>(ExposureMode::kAuto) == 1U);
static_assert(static_cast<std::uint32_t>(FrameFlag::kSourcePtsUnvalidated) == 1U);

struct Caps {
    std::uint32_t sensor_width = 0;
    std::uint32_t sensor_height = 0;
    std::uint32_t sensor_fps = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    ExposureMode exposure_mode = ExposureMode::kFixed;
};

struct Config {
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

struct FrameView {
    std::span<const std::byte> luma{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint64_t source_pts = 0;
    bool source_pts_timebase_validated = false;
    std::chrono::microseconds received{};
};

namespace detail {

struct FrameLeaseState;
using ReleaseFunction = std::expected<void, ErrorPtr> (*)(FrameLeaseState*) noexcept;
using FailureFunction = void (*)(FrameLeaseState*, ErrorPtr) noexcept;

struct FrameLeaseState {
    FrameView view{};
    void* owner = nullptr;
    std::uint64_t token = 0;
    const std::byte* data = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t flags = 0;
    std::uint64_t source_pts = 0;
    std::uint64_t received_us = 0;
    ReleaseFunction release = nullptr;
    FailureFunction remember_failure = nullptr;
    bool released = false;
};

}  // namespace detail

class FrameLease {
public:
    FrameLease() noexcept = default;
    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;
    ~FrameLease();

    const FrameView& view() const noexcept;
    std::expected<void, ErrorPtr> release() noexcept;
    bool valid() const noexcept { return state_ != nullptr && !state_->released; }

private:
    explicit FrameLease(std::unique_ptr<detail::FrameLeaseState> state) noexcept
        : state_(std::move(state)) {}

    std::unique_ptr<detail::FrameLeaseState> state_;
    friend class K230Camera;
};

struct ExposurePlan {
    std::uint32_t requested_us = 0;
    std::uint32_t applied_us = 0;
    float min_us = 0.0F;
    float max_us = 0.0F;
    float seconds = 0.0F;
};

std::expected<ExposurePlan, ErrorPtr> plan_fixed_exposure_us(
    std::uint32_t requested_us, float min_us, float max_us,
    Operation operation = Operation::kSetExposure) noexcept;

class ICamera {
public:
    virtual ~ICamera() = default;

    virtual std::expected<void, ErrorPtr> start() noexcept = 0;
    virtual std::expected<std::optional<FrameLease>, ErrorPtr> acquire(
        std::chrono::milliseconds timeout) noexcept = 0;
    virtual std::expected<void, ErrorPtr> stop() noexcept = 0;
    virtual State state() const noexcept = 0;
    virtual const Caps& caps() const noexcept = 0;
    virtual std::expected<void, ErrorPtr> set_fixed_exposure_us(
        std::uint32_t us) noexcept = 0;
};

std::expected<std::unique_ptr<ICamera>, ErrorPtr> create_k230_camera(
    const Config& config) noexcept;

}  // namespace dart::camera

#endif
