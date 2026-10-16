#ifndef DART_CAMERA_HPP
#define DART_CAMERA_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "camera/error.hpp"
#include "camera/frame_view.hpp"

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

/* Lease capacity is deliberately absent: the caller injects it as a
 * compile-time parameter of the concrete camera, so the compiler knows the bound
 * and the adapter needs no runtime allocation for its lease slots.
 */
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

namespace detail {

class FrameLeaseAccess;

enum class FrameLeaseSlotState : std::uint8_t {
    kFree,
    kHeld,
    /* The camera was destroyed while this lease was still held. The lease can no
     * longer be released; doing so reports a stale lease instead of calling into
     * a dead object.
     */
    kDetached,
};

struct FrameLeaseSlot;

using ReleaseFunction = std::expected<void, ErrorPtr> (*)(FrameLeaseSlot* slot) noexcept;

/* One lease slot. The concrete camera owns an array with exactly as many slots
 * as its capacity parameter, so a lease is a pointer into that array instead of
 * an allocation and the bound is known at compile time.
 */
struct FrameLeaseSlot {
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
    /* Set when a lease is destroyed without a successful release, so the owning
     * camera can retry it and report it instead of losing it.
     */
    ErrorPtr pending_error{};
    FrameLeaseSlotState state = FrameLeaseSlotState::kFree;
};

}  // namespace detail

/* Move-only RAII handle to one slot of the owning camera. Releasing twice, or
 * after the camera is gone, is reported rather than silently ignored.
 */
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
    bool valid() const noexcept;

private:
    explicit FrameLease(detail::FrameLeaseSlot* slot) noexcept : slot_(slot) {}

    detail::FrameLeaseSlot* slot_ = nullptr;
    friend class detail::FrameLeaseAccess;
};

struct AcquireResult {
    AcquireStatus status = AcquireStatus::kNoFrame;
    FrameLease lease{};
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
    /* Three outcomes, only one of which is an error: a frame, no frame, or a full
     * lease pool. A caller with its own bounded pipeline drops on
     * kLeaseExhausted and counts it.
     */
    virtual std::expected<AcquireResult, ErrorPtr> acquire(
        std::chrono::milliseconds timeout) noexcept = 0;
    virtual std::expected<void, ErrorPtr> stop() noexcept = 0;
    virtual State state() const noexcept = 0;
    virtual const Caps& caps() const noexcept = 0;
    virtual std::expected<void, ErrorPtr> set_fixed_exposure_us(
        std::uint32_t us) noexcept = 0;

    /* Lease slots fixed at creation, and how many are in use right now. A
     * producer can check these before acquiring to apply its own drop policy.
     */
    virtual std::uint32_t capacity() const noexcept = 0;
    virtual std::uint32_t outstanding() const noexcept = 0;
};

namespace detail {

/* Grants camera implementations access to a lease's private slot without naming
 * any concrete camera in this header.
 */
class FrameLeaseAccess {
public:
    static FrameLease make(FrameLeaseSlot* slot) noexcept { return FrameLease(slot); }
    static FrameLeaseSlot* slot_of(const FrameLease& lease) noexcept { return lease.slot_; }
};

}  // namespace detail

}  // namespace dart::camera

#endif
