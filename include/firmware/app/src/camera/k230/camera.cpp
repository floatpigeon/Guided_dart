#include "camera/k230/camera.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace dart::camera::k230 {

static_assert(static_cast<std::uint32_t>(State::kEmpty) == DART_CAMERA_STATE_EMPTY);
static_assert(static_cast<std::uint32_t>(State::kInitialized) ==
              DART_CAMERA_STATE_INITIALIZED);
static_assert(static_cast<std::uint32_t>(State::kStarted) == DART_CAMERA_STATE_STARTED);
static_assert(static_cast<std::uint32_t>(State::kFaulted) == DART_CAMERA_STATE_FAULTED);
static_assert(static_cast<std::uint32_t>(ExposureMode::kFixed) == DART_CAMERA_EXPOSURE_FIXED);
static_assert(static_cast<std::uint32_t>(ExposureMode::kAuto) == DART_CAMERA_EXPOSURE_AUTO);
static_assert(static_cast<std::uint32_t>(FrameFlag::kSourcePtsUnvalidated) ==
              DART_CAMERA_FRAME_SOURCE_PTS_UNVALIDATED);

namespace {

CallContext context_for(Operation operation) noexcept {
    CallContext context{};
    context.operation = operation;
    return context;
}

template <typename T>
std::expected<T, ErrorPtr> failure(std::int32_t code,
                                   const CallContext& context) noexcept {
    return std::unexpected<ErrorPtr>(translate_camera_code(code, context));
}

/* The lease capacity is injected here and nowhere else: the C data plane sizes
 * its slots from the same number, so both sides agree by construction.
 */
template <std::uint32_t kCapacity>
dart_camera_config_t to_c_config(const Config& config) noexcept {
    dart_camera_config_t result{};
    dart_camera_config_init(&result);
    result.csi_port = config.csi_port;
    result.sensor_width = config.sensor_width;
    result.sensor_height = config.sensor_height;
    result.sensor_fps = config.sensor_fps;
    result.output_width = config.output_width;
    result.output_height = config.output_height;
    result.capture_buffers = config.capture_buffers;
    result.channel_buffers = config.channel_buffers;
    result.max_outstanding_frames = kCapacity;
    result.exposure_mode = static_cast<std::uint32_t>(config.exposure_mode);
    /* Exposure policy lives in the C++ planner, not in the C data plane. */
    result.initial_exposure_us = 0;
    result.mirror = config.mirror;
    return result;
}

void fill_requested_sensor(CallContext& context, const Config& config) noexcept {
    context.has_sensor_mode = true;
    context.requested_sensor.csi_port = config.csi_port;
    context.requested_sensor.width = config.sensor_width;
    context.requested_sensor.height = config.sensor_height;
    context.requested_sensor.fps = config.sensor_fps;
}

template <std::uint32_t kCapacity>
std::expected<void, ErrorPtr> validate_config(const Config& config) noexcept {
    if (config.exposure_mode == ExposureMode::kAuto && config.initial_exposure_us != 0U) {
        CallContext context = context_for(Operation::kConfigValidate);
        context.requested = config.initial_exposure_us;
        fill_requested_sensor(context, config);
        return failure<void>(DART_CAMERA_E_CONFIG_INVALID, context);
    }
    /* Each held lease keeps one driver buffer, so the injected capacity cannot
     * exceed the channel pool. Report the pair instead of a bare code. */
    if (kCapacity > config.channel_buffers) {
        CallContext context = context_for(Operation::kConfigValidate);
        context.requested = kCapacity;
        context.observed = config.channel_buffers;
        fill_requested_sensor(context, config);
        return failure<void>(DART_CAMERA_E_CONFIG_INVALID, context);
    }
    const dart_camera_config_t c_config = to_c_config<kCapacity>(config);
    const int rc = dart_camera_config_validate(&c_config);
    if (rc != DART_CAMERA_OK) {
        CallContext context = context_for(Operation::kConfigValidate);
        context.native_hint = rc;
        fill_requested_sensor(context, config);
        return failure<void>(rc, context);
    }
    return {};
}

}  // namespace

template <std::uint32_t kCapacity>
std::expected<std::unique_ptr<Camera<kCapacity>>, ErrorPtr>
Camera<kCapacity>::create(const Config& config) noexcept {
    auto valid = validate_config<kCapacity>(config);
    if (!valid) return std::unexpected<ErrorPtr>(std::move(valid.error()));

    auto object = std::unique_ptr<Camera>(new (std::nothrow) Camera(config));
    if (object == nullptr) {
        return failure<std::unique_ptr<Camera>>(DART_CAMERA_E_NO_MEMORY,
                                                    context_for(Operation::kCreate));
    }

    dart_media_t* media = nullptr;
    int rc = dart_media_create(&media);
    object->media_ = media;
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kMediaCreate);
        context.native_hint = rc;
        (void)object->destroy_handles();
        return failure<std::unique_ptr<Camera>>(rc, context);
    }

    dart_camera_t* camera = nullptr;
    rc = dart_camera_create(&camera);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kCreate);
        context.native_hint = rc;
        (void)object->destroy_handles();
        return failure<std::unique_ptr<Camera>>(rc, context);
    }
    object->camera_ = camera;

    const dart_camera_config_t c_config = to_c_config<kCapacity>(config);
    rc = dart_camera_init(object->camera_, object->media_, &c_config);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kInitialize);
        context.native_hint = rc;
        fill_requested_sensor(context, config);
        (void)object->refresh_state(Operation::kInitialize);
        (void)object->destroy_handles();
        return failure<std::unique_ptr<Camera>>(rc, context);
    }
    object->state_ = State::kInitialized;
    auto caps = object->refresh_caps();
    if (!caps) {
        (void)object->destroy_handles();
        return std::unexpected<ErrorPtr>(std::move(caps.error()));
    }
    return object;
}

template <std::uint32_t kCapacity>
std::expected<void, ErrorPtr> Camera<kCapacity>::refresh_caps() noexcept {
    if (camera_ == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_STATE, context_for(Operation::kGetCaps));
    }
    dart_camera_caps_t value{};
    const int rc = dart_camera_get_caps(camera_, &value);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kGetCaps);
        context.native_hint = rc;
        return failure<void>(rc, context);
    }
    if (value.abi_version != DART_CAMERA_ABI_VERSION ||
        value.struct_size != sizeof(value) ||
        value.exposure_mode > DART_CAMERA_EXPOSURE_AUTO) {
        auto context = context_for(Operation::kGetCaps);
        context.observed = value.abi_version;
        return failure<void>(DART_CAMERA_E_CONFIG_VERSION, context);
    }
    caps_.sensor_width = value.sensor_width;
    caps_.sensor_height = value.sensor_height;
    caps_.sensor_fps = value.sensor_fps;
    caps_.output_width = value.output_width;
    caps_.output_height = value.output_height;
    caps_.exposure_mode = static_cast<ExposureMode>(value.exposure_mode);
    return {};
}

template <std::uint32_t kCapacity>
std::expected<void, ErrorPtr> Camera<kCapacity>::refresh_state(
    Operation operation) const noexcept {
    if (camera_ == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_STATE, context_for(operation));
    }
    std::uint32_t value = 0;
    const int rc = dart_camera_get_state(camera_, &value);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(operation);
        context.native_hint = rc;
        return failure<void>(rc, context);
    }
    if (value > DART_CAMERA_STATE_FAULTED) {
        auto context = context_for(operation);
        context.observed = value;
        return failure<void>(DART_CAMERA_E_INVALID_STATE, context);
    }
    state_ = static_cast<State>(value);
    return {};
}

template <std::uint32_t kCapacity>
std::expected<void, ErrorPtr> Camera<kCapacity>::start() noexcept {
    auto pending = retry_pending_releases();
    if (!pending) return pending;
    if (camera_ == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_STATE, context_for(Operation::kStart));
    }
    int rc = dart_camera_start(camera_);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kStart);
        context.native_hint = rc;
        (void)refresh_state(Operation::kStart);
        context.rollback_failed = state_ == State::kFaulted;
        return failure<void>(rc, context);
    }
    state_ = State::kStarted;
    if (config_.exposure_mode == ExposureMode::kFixed &&
        config_.initial_exposure_us != 0U) {
        auto applied = set_fixed_exposure_us(config_.initial_exposure_us);
        if (!applied) {
            ErrorPtr original = std::move(applied.error());
            const int stop_rc = dart_camera_stop(camera_);
            state_ = stop_rc == DART_CAMERA_OK ? State::kInitialized : State::kFaulted;
            if (original != nullptr && stop_rc == DART_CAMERA_OK) {
                return std::unexpected<ErrorPtr>(std::move(original));
            }
            if (original != nullptr) {
                auto context = context_for(Operation::kStart);
                context.native_hint = original->code();
                context.rollback_failed = true;
                return failure<void>(original->code(), context);
            }
            auto context = context_for(Operation::kStart);
            context.rollback_failed = state_ == State::kFaulted;
            return failure<void>(DART_CAMERA_E_EXPOSURE_WRITE_FAILED, context);
        }
    }
    return {};
}

template <std::uint32_t kCapacity>
std::uint32_t Camera<kCapacity>::outstanding() const noexcept {
    std::uint32_t count = 0;
    for (const auto& slot : slots_) {
        if (slot.state != detail::FrameLeaseSlotState::kFree) ++count;
    }
    return count;
}

template <std::uint32_t kCapacity>
detail::FrameLeaseSlot* Camera<kCapacity>::free_slot() noexcept {
    for (auto& slot : slots_) {
        if (slot.state == detail::FrameLeaseSlotState::kFree) return &slot;
    }
    return nullptr;
}

template <std::uint32_t kCapacity>
void Camera<kCapacity>::fill_slot(detail::FrameLeaseSlot* slot,
                                      const dart_camera_frame_t& frame) noexcept {
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(frame.stride) * static_cast<std::uint64_t>(frame.height);
    slot->token = frame.token;
    slot->data = reinterpret_cast<const std::byte*>(frame.data);
    slot->width = frame.width;
    slot->height = frame.height;
    slot->stride = frame.stride;
    slot->flags = frame.flags;
    slot->source_pts = frame.source_pts;
    slot->received_us = frame.received_us;
    slot->view.luma = std::span<const std::byte>(slot->data, static_cast<std::size_t>(bytes));
    slot->view.width = frame.width;
    slot->view.height = frame.height;
    slot->view.stride = frame.stride;
    slot->view.source_pts = frame.source_pts;
    slot->view.sequence = frame.token;
    slot->view.source_pts_timebase_validated =
        (frame.flags & DART_CAMERA_FRAME_SOURCE_PTS_UNVALIDATED) == 0U;
    slot->view.received = std::chrono::microseconds(frame.received_us);
    slot->owner = this;
    slot->release = &Camera::slot_release;
    slot->pending_error.reset();
    slot->state = detail::FrameLeaseSlotState::kHeld;
}

template <std::uint32_t kCapacity>
std::expected<AcquireResult, ErrorPtr> Camera<kCapacity>::acquire(
    std::chrono::milliseconds timeout) noexcept {
    /* A release that failed earlier is retried before anything else: a stuck
     * lease keeps its slot, and reporting it immediately is more useful than
     * letting the pipeline wonder why capacity shrank.
     */
    auto retried = retry_pending_releases();
    if (!retried) return std::unexpected<ErrorPtr>(std::move(retried.error()));

    const auto count = timeout.count();
    if (count < 0 || count > 1000) {
        auto context = context_for(Operation::kAcquire);
        context.requested = count < 0 ? 0U : static_cast<std::uint32_t>(count);
        return failure<AcquireResult>(DART_CAMERA_E_OUT_OF_RANGE, context);
    }
    if (camera_ == nullptr) {
        return failure<AcquireResult>(DART_CAMERA_E_INVALID_STATE,
                                      context_for(Operation::kAcquire));
    }

    /* Bounded capacity: when the pipeline holds every slot, the driver is not
     * touched at all, so the frame stays in its pool and is recycled there. That
     * is the drop signal, not an error.
     */
    detail::FrameLeaseSlot* slot = free_slot();
    if (slot == nullptr) {
        return AcquireResult{AcquireStatus::kLeaseExhausted, FrameLease{}};
    }

    dart_camera_frame_t frame{};
    const int rc = dart_camera_acquire_frame(camera_, static_cast<std::uint32_t>(count), &frame);
    if (rc == DART_CAMERA_E_NO_FRAME) {
        return AcquireResult{AcquireStatus::kNoFrame, FrameLease{}};
    }
    if (rc == DART_CAMERA_E_LEASE_EXHAUSTED) {
        /* The C pool is sized from the same capacity, so this only means the two
         * sides disagree; it is still a drop, not a failure. */
        return AcquireResult{AcquireStatus::kLeaseExhausted, FrameLease{}};
    }
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kAcquire);
        context.native_hint = rc;
        context.observed = outstanding();
        return failure<AcquireResult>(rc, context);
    }

    const std::uint64_t bytes =
        static_cast<std::uint64_t>(frame.stride) * static_cast<std::uint64_t>(frame.height);
    if (bytes > std::numeric_limits<std::size_t>::max()) {
        (void)dart_camera_release_frame(camera_, &frame);
        return failure<AcquireResult>(DART_CAMERA_E_OUT_OF_RANGE,
                                      context_for(Operation::kAcquire));
    }
    fill_slot(slot, frame);
    return AcquireResult{AcquireStatus::kFrame, detail::FrameLeaseAccess::make(slot)};
}

template <std::uint32_t kCapacity>
std::expected<void, ErrorPtr> Camera<kCapacity>::slot_release(
    detail::FrameLeaseSlot* slot) noexcept {
    if (slot == nullptr || slot->owner == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_ARGUMENT, context_for(Operation::kRelease));
    }
    auto* self = static_cast<Camera*>(slot->owner);
    return self->release_slot(slot);
}

template <std::uint32_t kCapacity>
std::expected<void, ErrorPtr> Camera<kCapacity>::release_slot(
    detail::FrameLeaseSlot* slot) noexcept {
    if (slot == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_ARGUMENT, context_for(Operation::kRelease));
    }
    if (camera_ == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_STATE, context_for(Operation::kRelease));
    }
    dart_camera_frame_t frame{};
    frame.abi_version = DART_CAMERA_ABI_VERSION;
    frame.struct_size = sizeof(frame);
    frame.data = reinterpret_cast<const std::uint8_t*>(slot->data);
    frame.width = slot->width;
    frame.height = slot->height;
    frame.stride = slot->stride;
    frame.flags = slot->flags;
    frame.source_pts = slot->source_pts;
    frame.received_us = slot->received_us;
    frame.token = slot->token;
    frame.owner = camera_;
    const int rc = dart_camera_release_frame(camera_, &frame);
    if (rc == DART_CAMERA_OK) {
        /* Hand the slot back empty: a stale lease can no longer point at any
         * frame data, and the next acquire may reuse it with a fresh token. */
        *slot = detail::FrameLeaseSlot{};
        return {};
    }
    auto context = context_for(Operation::kRelease);
    context.native_hint = rc;
    context.requested = static_cast<std::uint32_t>(slot->token);
    context.lease_token = slot->token;
    context.lease_generation = slot->token;
    context.lease_owner = reinterpret_cast<std::uintptr_t>(camera_);
    return failure<void>(rc, context);
}

template <std::uint32_t kCapacity>
std::expected<void, ErrorPtr> Camera<kCapacity>::retry_pending_releases() noexcept {
    for (auto& slot : slots_) {
        if (slot.state != detail::FrameLeaseSlotState::kHeld) continue;
        if (slot.pending_error == nullptr) continue;
        auto retried = release_slot(&slot);
        if (!retried) return retried;
    }
    return {};
}

template <std::uint32_t kCapacity>
std::expected<void, ErrorPtr> Camera<kCapacity>::set_fixed_exposure_us(
    std::uint32_t us) noexcept {
    auto pending = retry_pending_releases();
    if (!pending) return pending;
    if (camera_ == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_STATE, context_for(Operation::kSetExposure));
    }
    if (state_ != State::kStarted) {
        auto context = context_for(Operation::kSetExposure);
        context.state = static_cast<std::uint32_t>(state_);
        context.required_state = static_cast<std::uint32_t>(State::kStarted);
        return failure<void>(DART_CAMERA_E_INVALID_STATE, context);
    }
    if (config_.exposure_mode == ExposureMode::kAuto) {
        return failure<void>(DART_CAMERA_E_NOT_SUPPORTED, context_for(Operation::kSetExposure));
    }
    float min_us = 0.0F;
    float max_us = 0.0F;
    int rc = dart_camera_get_exposure_range(camera_, &min_us, &max_us);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kExposureRange);
        context.native_hint = rc;
        context.has_exposure = true;
        context.requested_us = static_cast<float>(us);
        context.min_us = min_us;
        context.max_us = max_us;
        return failure<void>(rc, context);
    }
    auto plan = plan_fixed_exposure_us(us, min_us, max_us, Operation::kSetExposure);
    if (!plan) return std::unexpected<ErrorPtr>(std::move(plan.error()));
    auto context = context_for(Operation::kSetExposure);
    context.has_exposure = true;
    context.requested_us = static_cast<float>(us);
    context.min_us = min_us;
    context.max_us = max_us;
    context.applied_us = static_cast<float>(plan->applied_us);
    rc = dart_camera_set_exposure_seconds(camera_, plan->seconds);
    if (rc != DART_CAMERA_OK) {
        context.native_hint = rc;
        return failure<void>(rc, context);
    }
    return {};
}

template <std::uint32_t kCapacity>
std::expected<void, ErrorPtr> Camera<kCapacity>::stop() noexcept {
    auto pending = retry_pending_releases();
    if (!pending) return pending;
    if (camera_ == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_STATE, context_for(Operation::kStop));
    }
    /* The C layer refuses to stop while any lease is outstanding, so a caller
     * that still holds frames gets BUSY here rather than a silently dead stream. */
    const int rc = dart_camera_stop(camera_);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kStop);
        context.native_hint = rc;
        context.observed = outstanding();
        (void)refresh_state(Operation::kStop);
        return failure<void>(rc, context);
    }
    state_ = State::kInitialized;
    return {};
}

template <std::uint32_t kCapacity>
std::expected<void, ErrorPtr> Camera<kCapacity>::destroy_handles() noexcept {
    auto pending = retry_pending_releases();
    if (!pending) return pending;
    if (camera_ != nullptr) {
        auto observed = refresh_state(Operation::kGetState);
        if (!observed) return observed;

        if (state_ == State::kStarted) {
            const int rc = dart_camera_stop(camera_);
            if (rc != DART_CAMERA_OK) {
                auto context = context_for(Operation::kStop);
                context.native_hint = rc;
                state_ = State::kFaulted;
                return failure<void>(rc, context);
            }
            state_ = State::kInitialized;
        }
        if (state_ != State::kEmpty) {
            const int rc = dart_camera_deinit(camera_);
            if (rc != DART_CAMERA_OK) {
                auto context = context_for(Operation::kDeinitialize);
                context.native_hint = rc;
                (void)refresh_state(Operation::kDeinitialize);
                return failure<void>(rc, context);
            }
            state_ = State::kEmpty;
        }
        const int rc = dart_camera_destroy(camera_);
        if (rc != DART_CAMERA_OK) {
            auto context = context_for(Operation::kDestroy);
            context.native_hint = rc;
            return failure<void>(rc, context);
        }
        camera_ = nullptr;
    }
    if (media_ != nullptr) {
        const int rc = dart_media_destroy(media_);
        if (rc != DART_CAMERA_OK) {
            auto context = context_for(Operation::kMediaDestroy);
            context.native_hint = rc;
            return failure<void>(rc, context);
        }
        media_ = nullptr;
    }
    return {};
}

template <std::uint32_t kCapacity>
void Camera<kCapacity>::detach_leases() noexcept {
    for (auto& slot : slots_) {
        if (slot.state == detail::FrameLeaseSlotState::kFree) continue;
        /* The C layer will not release a frame while the caller still holds the
         * lease, so this path cannot free the dump. Detaching keeps a later
         * release from calling into a destroyed object; the caller gets a stale
         * lease instead, and the SDK handles stay for an explicit retry.
         */
        slot.state = detail::FrameLeaseSlotState::kDetached;
        slot.owner = nullptr;
        slot.release = nullptr;
    }
}

template <std::uint32_t kCapacity>
Camera<kCapacity>::~Camera() {
    detach_leases();
    (void)destroy_handles();
}

/* Every capacity the C ABI allows is instantiated here, so the upper layer can
 * pick one at its call site. Raising DART_CAMERA_MAX_OUTSTANDING_FRAMES needs the
 * matching line below.
 */
template class Camera<1U>;
template class Camera<2U>;
template class Camera<3U>;
template class Camera<4U>;
template class Camera<5U>;
template class Camera<6U>;
template class Camera<7U>;
template class Camera<8U>;
template class Camera<9U>;
template class Camera<10U>;

}  // namespace dart::camera::k230
