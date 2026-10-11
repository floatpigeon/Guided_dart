#include "k230_camera.hpp"

#include "camera.h"

#include <cstddef>
#include <limits>
#include <new>
#include <utility>

namespace dart::camera {

static_assert(static_cast<std::uint32_t>(State::kEmpty) == DART_CAMERA_STATE_EMPTY);
static_assert(static_cast<std::uint32_t>(State::kInitialized) == DART_CAMERA_STATE_INITIALIZED);
static_assert(static_cast<std::uint32_t>(State::kStarted) == DART_CAMERA_STATE_STARTED);
static_assert(static_cast<std::uint32_t>(State::kFaulted) == DART_CAMERA_STATE_FAULTED);
static_assert(static_cast<std::uint32_t>(ExposureMode::kFixed) == DART_CAMERA_EXPOSURE_FIXED);
static_assert(static_cast<std::uint32_t>(ExposureMode::kAuto) == DART_CAMERA_EXPOSURE_AUTO);
static_assert(static_cast<std::uint32_t>(FrameFlag::kSourcePtsUnvalidated) == DART_CAMERA_FRAME_SOURCE_PTS_UNVALIDATED);

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

std::expected<void, ErrorPtr> validate_config(const Config& config) noexcept {
    if (config.exposure_mode == ExposureMode::kAuto && config.initial_exposure_us != 0U) {
        CallContext context = context_for(Operation::kConfigValidate);
        context.requested = config.initial_exposure_us;
        fill_requested_sensor(context, config);
        return failure<void>(DART_CAMERA_E_CONFIG_INVALID, context);
    }
    const dart_camera_config_t c_config = to_c_config(config);
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

std::expected<std::unique_ptr<K230Camera>, ErrorPtr> K230Camera::create(
    const Config& config) noexcept {
    auto valid = validate_config(config);
    if (!valid) return std::unexpected<ErrorPtr>(std::move(valid.error()));

    auto object = std::unique_ptr<K230Camera>(new (std::nothrow) K230Camera(config));
    if (object == nullptr) {
        return failure<std::unique_ptr<K230Camera>>(
            DART_CAMERA_E_NO_MEMORY, context_for(Operation::kCreate));
    }

    dart_media_t* media = nullptr;
    int rc = dart_media_create(&media);
    object->media_ = media;
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kMediaCreate);
        context.native_hint = rc;
        (void)object->destroy_handles();
        return failure<std::unique_ptr<K230Camera>>(rc, context);
    }

    dart_camera_t* camera = nullptr;
    rc = dart_camera_create(&camera);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kCreate);
        context.native_hint = rc;
        (void)object->destroy_handles();
        return failure<std::unique_ptr<K230Camera>>(rc, context);
    }
    object->camera_ = camera;

    const dart_camera_config_t c_config = to_c_config(config);
    rc = dart_camera_init(object->camera_, object->media_, &c_config);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kInitialize);
        context.native_hint = rc;
        fill_requested_sensor(context, config);
        (void)object->refresh_state(Operation::kInitialize);
        (void)object->destroy_handles();
        return failure<std::unique_ptr<K230Camera>>(rc, context);
    }
    object->state_ = State::kInitialized;
    auto caps = object->refresh_caps();
    if (!caps) {
        (void)object->destroy_handles();
        return std::unexpected<ErrorPtr>(std::move(caps.error()));
    }
    return object;
}

std::expected<void, ErrorPtr> K230Camera::refresh_caps() noexcept {
    if (camera_ == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_STATE,
                             context_for(Operation::kGetCaps));
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

std::expected<void, ErrorPtr> K230Camera::refresh_state(Operation operation) const noexcept {
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

std::expected<void, ErrorPtr> K230Camera::start() noexcept {
    auto pending = retry_pending_release();
    if (!pending) return pending;
    if (camera_ == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_STATE,
                             context_for(Operation::kStart));
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

std::expected<std::optional<FrameLease>, ErrorPtr> K230Camera::acquire(
    std::chrono::milliseconds timeout) noexcept {
    if (has_pending_release()) {
        return std::unexpected<ErrorPtr>(pending_release_busy(Operation::kAcquire));
    }
    const auto count = timeout.count();
    if (count < 0 || count > 1000) {
        auto context = context_for(Operation::kAcquire);
        context.requested = count < 0 ? 0U : static_cast<std::uint32_t>(count);
        return failure<std::optional<FrameLease>>(DART_CAMERA_E_OUT_OF_RANGE, context);
    }
    if (camera_ == nullptr) {
        return failure<std::optional<FrameLease>>(
            DART_CAMERA_E_INVALID_STATE, context_for(Operation::kAcquire));
    }

    dart_camera_frame_t frame{};
    const int rc = dart_camera_acquire_frame(
        camera_, static_cast<std::uint32_t>(count), &frame);
    if (rc == DART_CAMERA_E_NO_FRAME) return std::optional<FrameLease>{};
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kAcquire);
        context.native_hint = rc;
        return failure<std::optional<FrameLease>>(rc, context);
    }

    auto retain_failed_release = [this, &frame](int release_rc) noexcept {
        if (release_rc == DART_CAMERA_OK) return;
        CallContext release_context = context_for(Operation::kRelease);
        release_context.native_hint = release_rc;
        release_context.lease_token = frame.token;
        release_context.lease_generation = frame.token;
        release_context.lease_owner = reinterpret_cast<std::uintptr_t>(this);
        pending_release_error_ = translate_camera_code(release_rc, release_context);
        pending_frame_.data = reinterpret_cast<const std::byte*>(frame.data);
        pending_frame_.width = frame.width;
        pending_frame_.height = frame.height;
        pending_frame_.stride = frame.stride;
        pending_frame_.flags = frame.flags;
        pending_frame_.source_pts = frame.source_pts;
        pending_frame_.received_us = frame.received_us;
        pending_frame_.token = frame.token;
        pending_frame_.valid = true;
    };

    const std::uint64_t bytes = static_cast<std::uint64_t>(frame.stride) * frame.height;
    if (bytes > std::numeric_limits<std::size_t>::max()) {
        retain_failed_release(dart_camera_release_frame(camera_, &frame));
        return failure<std::optional<FrameLease>>(
            DART_CAMERA_E_OUT_OF_RANGE, context_for(Operation::kAcquire));
    }
    auto state = std::unique_ptr<detail::FrameLeaseState>(
        new (std::nothrow) detail::FrameLeaseState);
    if (state == nullptr) {
        retain_failed_release(dart_camera_release_frame(camera_, &frame));
        return failure<std::optional<FrameLease>>(
            DART_CAMERA_E_NO_MEMORY, context_for(Operation::kAcquire));
    }
    state->owner = this;
    state->token = frame.token;
    state->data = reinterpret_cast<const std::byte*>(frame.data);
    state->width = frame.width;
    state->height = frame.height;
    state->stride = frame.stride;
    state->flags = frame.flags;
    state->source_pts = frame.source_pts;
    state->received_us = frame.received_us;
    state->view.luma = std::span<const std::byte>(state->data, static_cast<std::size_t>(bytes));
    state->view.width = frame.width;
    state->view.height = frame.height;
    state->view.stride = frame.stride;
    state->view.source_pts = frame.source_pts;
    state->view.source_pts_timebase_validated =
        (frame.flags & DART_CAMERA_FRAME_SOURCE_PTS_UNVALIDATED) == 0U;
    state->view.received = std::chrono::microseconds(frame.received_us);
    state->release = &K230Camera::release_lease;
    state->remember_failure = &K230Camera::remember_release_failure;
    return std::optional<FrameLease>(std::in_place, FrameLease(std::move(state)));
}

std::expected<void, ErrorPtr> K230Camera::release_lease(
    detail::FrameLeaseState* state) noexcept {
    if (state == nullptr || state->owner == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_ARGUMENT,
                             context_for(Operation::kRelease));
    }
    auto* self = static_cast<K230Camera*>(state->owner);
    if (self->camera_ == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_STATE,
                             context_for(Operation::kRelease));
    }
    dart_camera_frame_t frame{};
    frame.abi_version = DART_CAMERA_ABI_VERSION;
    frame.struct_size = sizeof(frame);
    frame.data = reinterpret_cast<const std::uint8_t*>(state->data);
    frame.width = state->width;
    frame.height = state->height;
    frame.stride = state->stride;
    frame.flags = state->flags;
    frame.source_pts = state->source_pts;
    frame.received_us = state->received_us;
    frame.token = state->token;
    frame.owner = self->camera_;
    const int rc = dart_camera_release_frame(self->camera_, &frame);
    if (rc == DART_CAMERA_OK) return {};
    auto context = context_for(Operation::kRelease);
    context.native_hint = rc;
    context.requested = static_cast<std::uint32_t>(state->token);
    context.lease_token = state->token;
    context.lease_generation = state->token;
    context.lease_owner = reinterpret_cast<std::uintptr_t>(state->owner);
    return failure<void>(rc, context);
}

void K230Camera::remember_release_failure(detail::FrameLeaseState* state,
                                          ErrorPtr error) noexcept {
    if (state == nullptr || state->owner == nullptr) return;
    auto* self = static_cast<K230Camera*>(state->owner);
    self->pending_release_error_ = std::move(error);
    self->pending_frame_.data = state->data;
    self->pending_frame_.width = state->width;
    self->pending_frame_.height = state->height;
    self->pending_frame_.stride = state->stride;
    self->pending_frame_.flags = state->flags;
    self->pending_frame_.source_pts = state->source_pts;
    self->pending_frame_.received_us = state->received_us;
    self->pending_frame_.token = state->token;
    self->pending_frame_.valid = true;
}

std::expected<void, ErrorPtr> K230Camera::set_fixed_exposure_us(
    std::uint32_t us) noexcept {
    if (has_pending_release()) {
        return std::unexpected<ErrorPtr>(pending_release_busy(Operation::kSetExposure));
    }
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

std::expected<void, ErrorPtr> K230Camera::retry_pending_release() noexcept {
    if (!pending_frame_.valid || camera_ == nullptr) return {};
    dart_camera_frame_t frame{};
    frame.abi_version = DART_CAMERA_ABI_VERSION;
    frame.struct_size = sizeof(frame);
    frame.data = reinterpret_cast<const std::uint8_t*>(pending_frame_.data);
    frame.width = pending_frame_.width;
    frame.height = pending_frame_.height;
    frame.stride = pending_frame_.stride;
    frame.flags = pending_frame_.flags;
    frame.source_pts = pending_frame_.source_pts;
    frame.received_us = pending_frame_.received_us;
    frame.token = pending_frame_.token;
    frame.owner = camera_;
    const int rc = dart_camera_release_frame(camera_, &frame);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kRelease);
        context.native_hint = rc;
        context.lease_token = pending_frame_.token;
        context.lease_generation = pending_frame_.token;
        context.lease_owner = reinterpret_cast<std::uintptr_t>(camera_);
        return failure<void>(rc, context);
    }
    pending_frame_ = {};
    pending_release_error_.reset();
    return {};
}

std::expected<void, ErrorPtr> K230Camera::stop() noexcept {
    auto pending = retry_pending_release();
    if (!pending) return pending;
    if (camera_ == nullptr) {
        return failure<void>(DART_CAMERA_E_INVALID_STATE,
                             context_for(Operation::kStop));
    }
    const int rc = dart_camera_stop(camera_);
    if (rc != DART_CAMERA_OK) {
        auto context = context_for(Operation::kStop);
        context.native_hint = rc;
        (void)refresh_state(Operation::kStop);
        return failure<void>(rc, context);
    }
    state_ = State::kInitialized;
    return {};
}

ErrorPtr K230Camera::pending_release_busy(Operation operation) const noexcept {
    CallContext context = context_for(operation);
    context.previous_release_code = pending_release_error_ == nullptr
                                        ? 0
                                        : pending_release_error_->code();
    context.lease_token = pending_frame_.token;
    context.lease_generation = pending_frame_.token;
    context.lease_owner = reinterpret_cast<std::uintptr_t>(camera_);
    return ErrorPtr(new (std::nothrow) BusyError(DART_CAMERA_E_BUSY, context));
}

std::expected<void, ErrorPtr> K230Camera::destroy_handles() noexcept {
    auto pending = retry_pending_release();
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

K230Camera::~K230Camera() {
    (void)destroy_handles();
}

std::expected<std::unique_ptr<ICamera>, ErrorPtr> create_k230_camera(
    const Config& config) noexcept {
    auto created = K230Camera::create(config);
    if (!created) return std::unexpected<ErrorPtr>(std::move(created.error()));
    std::unique_ptr<ICamera> result(std::move(created.value()));
    return result;
}

}  // namespace dart::camera
