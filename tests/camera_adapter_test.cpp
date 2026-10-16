

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

#include "camera/k230/camera.cpp"

namespace dart::tests {

// Replace only the C hardware boundary. The tests exercise the real C++ camera,
// including the release callback stored by a lease and startup rollback.
struct CameraFake {
    int media_handle = 0;
    int camera_handle = 0;
    bool media_open = false;
    bool camera_open = false;
    bool held = false;
    std::uint32_t state = DART_CAMERA_STATE_EMPTY;
    dart_camera_config_t config{};
    std::array<std::uint8_t, 320 * 240> pixels{};
    std::uint64_t token = 0;
    int acquire_calls = 0;
    int release_calls = 0;
    int release_failures = 0;
    int acquire_result = DART_CAMERA_OK;
    int exposure_result = DART_CAMERA_OK;
    int stop_result = DART_CAMERA_OK;
    float min_exposure_us = 10.2F;
    float max_exposure_us = 1000.8F;
    float applied_seconds = 0.0F;
    int exposure_calls = 0;
};

CameraFake fake;

extern "C" {
int dart_media_create(dart_media_t** media) {
    fake.media_open = true;
    *media = reinterpret_cast<dart_media_t*>(&fake.media_handle);
    return DART_CAMERA_OK;
}

int dart_media_destroy(dart_media_t*) {
    assert(!fake.camera_open);
    fake.media_open = false;
    return DART_CAMERA_OK;
}

int dart_camera_create(dart_camera_t** camera) {
    fake.camera_open = true;
    *camera = reinterpret_cast<dart_camera_t*>(&fake.camera_handle);
    return DART_CAMERA_OK;
}

int dart_camera_init(dart_camera_t*, dart_media_t*, const dart_camera_config_t* config) {
    fake.config = *config;
    fake.state = DART_CAMERA_STATE_INITIALIZED;
    return DART_CAMERA_OK;
}

int dart_camera_start(dart_camera_t*) {
    fake.state = DART_CAMERA_STATE_STARTED;
    return DART_CAMERA_OK;
}

int dart_camera_acquire_frame(dart_camera_t* camera, std::uint32_t, dart_camera_frame_t* frame) {
    ++fake.acquire_calls;
    if (fake.acquire_result != DART_CAMERA_OK)
        return fake.acquire_result;
    assert(!fake.held);
    fake.held = true;
    *frame = {};
    frame->data = fake.pixels.data();
    frame->width = frame->stride = 320;
    frame->height = 240;
    frame->token = ++fake.token;
    frame->owner = camera;
    return DART_CAMERA_OK;
}

int dart_camera_release_frame(dart_camera_t*, const dart_camera_frame_t* frame) {
    ++fake.release_calls;
    assert(fake.held && frame->token == fake.token);
    if (fake.release_failures > 0) {
        --fake.release_failures;
        return DART_CAMERA_E_BLOCK_RELEASE_FAILED;
    }
    fake.held = false;
    return DART_CAMERA_OK;
}

int dart_camera_stop(dart_camera_t*) {
    if (fake.held)
        return DART_CAMERA_E_BUSY;
    if (fake.stop_result != DART_CAMERA_OK)
        return fake.stop_result;
    fake.state = DART_CAMERA_STATE_INITIALIZED;
    return DART_CAMERA_OK;
}

int dart_camera_deinit(dart_camera_t*) {
    assert(!fake.held);
    fake.state = DART_CAMERA_STATE_EMPTY;
    return DART_CAMERA_OK;
}

int dart_camera_destroy(dart_camera_t*) {
    assert(fake.state == DART_CAMERA_STATE_EMPTY);
    fake.camera_open = false;
    return DART_CAMERA_OK;
}

int dart_camera_get_state(const dart_camera_t*, std::uint32_t* state) {
    *state = fake.state;
    return DART_CAMERA_OK;
}

int dart_camera_get_caps(const dart_camera_t*, dart_camera_caps_t* caps) {
    *caps = {};
    caps->abi_version = DART_CAMERA_ABI_VERSION;
    caps->struct_size = sizeof(*caps);
    caps->sensor_width = fake.config.sensor_width;
    caps->sensor_height = fake.config.sensor_height;
    caps->sensor_fps = fake.config.sensor_fps;
    caps->output_width = fake.config.output_width;
    caps->output_height = fake.config.output_height;
    caps->exposure_mode = fake.config.exposure_mode;
    return DART_CAMERA_OK;
}

int dart_camera_get_exposure_range(const dart_camera_t*, float* min_us, float* max_us) {
    *min_us = fake.min_exposure_us;
    *max_us = fake.max_exposure_us;
    return DART_CAMERA_OK;
}

int dart_camera_set_exposure_seconds(dart_camera_t*, float seconds) {
    fake.applied_seconds = seconds;
    ++fake.exposure_calls;
    return fake.exposure_result;
}
} // extern "C"

camera::Config camera_config() {
    camera::Config config{};
    config.sensor_width = 640;
    config.sensor_height = 480;
    config.sensor_fps = 30;
    config.output_width = 320;
    config.output_height = 240;
    config.capture_buffers = config.channel_buffers = 3;
    config.initial_exposure_us = 100;
    return config;
}

void release_retry() {
    fake = {};
    auto created = camera::k230::Camera<1>::create(camera_config());
    assert(created);
    auto& camera = *created.value();
    assert(camera.start());
    {
        auto frame = camera.acquire(std::chrono::milliseconds{0});
        assert(frame && frame->status == camera::AcquireStatus::kFrame);
        assert(camera.outstanding() == 1);
        const auto calls = fake.acquire_calls;
        auto exhausted = camera.acquire(std::chrono::milliseconds{0});
        assert(exhausted && exhausted->status == camera::AcquireStatus::kLeaseExhausted);
        assert(fake.acquire_calls == calls);
        auto busy = camera.stop();
        assert(!busy && busy.error().kind() == camera::ErrorKind::kBusy);
        // The destructor cannot report its failed release. It leaves the error
        // with the camera, which retries before allowing another acquisition.
        fake.release_failures = 2;
    }
    assert(camera.outstanding() == 1 && fake.release_calls == 1);
    auto failed = camera.acquire(std::chrono::milliseconds{0});
    assert(!failed && failed.error().kind() == camera::ErrorKind::kRelease);
    assert(failed.error().operation() == camera::Operation::kRelease);
    assert(failed.error().context().lease_token == 1);
    assert(fake.acquire_calls == 1 && fake.release_calls == 2);
    auto recovered = camera.acquire(std::chrono::milliseconds{0});
    assert(recovered && recovered->status == camera::AcquireStatus::kFrame);
    assert(fake.acquire_calls == 2 && fake.release_calls == 3);
    assert(recovered->lease.release());
    assert(camera.outstanding() == 0);
    fake.acquire_result = DART_CAMERA_E_NO_FRAME;
    auto empty = camera.acquire(std::chrono::milliseconds{0});
    assert(empty && empty->status == camera::AcquireStatus::kNoFrame);
    assert(camera.stop());
    created.value().reset();
    assert(!fake.camera_open && !fake.media_open);
}

void exposure_rollback(bool fail_stop) {
    fake = {};
    auto created = camera::k230::Camera<1>::create(camera_config());
    assert(created);
    fake.exposure_result = DART_CAMERA_E_EXPOSURE_WRITE_FAILED;
    fake.stop_result = fail_stop ? DART_CAMERA_E_BUSY : DART_CAMERA_OK;
    auto started = created.value()->start();
    assert(!started);
    const auto& error = started.error();
    assert(error.code() == DART_CAMERA_E_EXPOSURE_WRITE_FAILED);
    assert(error.kind() == camera::ErrorKind::kExposure);
    assert(error.context().rollback_failed == fail_stop);
    if (fail_stop) {
        assert(created.value()->state() == camera::State::kFaulted);
        assert(error.operation() == camera::Operation::kStart);
        assert(error.context().native_hint == DART_CAMERA_E_EXPOSURE_WRITE_FAILED);
    } else {
        assert(created.value()->state() == camera::State::kInitialized);
        assert(error.operation() == camera::Operation::kSetExposure);
        assert(error.context().has_exposure && error.context().requested_us == 100.0F);
    }
    fake.stop_result = DART_CAMERA_OK;
    created.value().reset();
    assert(!fake.camera_open && !fake.media_open);
}

void exposure_adjustment() {
    fake = {};
    auto created = camera::k230::Camera<1>::create(camera_config());
    assert(created && created.value()->start());
    auto& camera = *created.value();
    assert(std::abs(fake.applied_seconds - 0.000100F) < 1e-9F);
    fake.max_exposure_us = 20.8F;
    assert(camera.set_fixed_exposure_us(1));
    assert(std::abs(fake.applied_seconds - 0.000011F) < 1e-9F);
    assert(camera.set_fixed_exposure_us(99));
    assert(std::abs(fake.applied_seconds - 0.000020F) < 1e-9F);
    assert(camera.set_fixed_exposure_us(15));
    assert(std::abs(fake.applied_seconds - 0.000015F) < 1e-9F);

    const std::array<std::array<float, 2>, 7> invalid_ranges{{
        {20.0F, 10.0F}, {10.2F, 10.8F}, {-1.0F, 10.0F},
        {NAN, 10.0F}, {0.0F, NAN}, {0.0F, INFINITY}, {0.0F, 4294967296.0F}}};
    for (const auto& range : invalid_ranges) {
        fake.min_exposure_us = range[0];
        fake.max_exposure_us = range[1];
        const auto calls = fake.exposure_calls;
        auto adjusted = camera.set_fixed_exposure_us(10);
        assert(!adjusted && fake.exposure_calls == calls);
        const auto& error = adjusted.error();
        assert(error.kind() == camera::ErrorKind::kExposure);
        assert(error.code() == DART_CAMERA_E_OUT_OF_RANGE);
        assert(error.operation() == camera::Operation::kSetExposure);
        assert(error.context().has_exposure && error.context().requested_us == 10.0F);
        assert((std::isnan(range[0]) && std::isnan(error.context().min_us))
               || error.context().min_us == range[0]);
    }
}

} // namespace dart::tests

int main() {
    dart::tests::release_retry();
    dart::tests::exposure_adjustment();
    dart::tests::exposure_rollback(false);
    dart::tests::exposure_rollback(true);
}
