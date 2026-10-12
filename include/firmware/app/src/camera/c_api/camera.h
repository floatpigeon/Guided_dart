#ifndef DART_CAMERA_H
#define DART_CAMERA_H

#include <stdint.h>

#include "camera/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DART_CAMERA_ABI_VERSION 3u
#define DART_CAMERA_MAX_BUFFERS 10u
/* Ceiling for config.max_outstanding_frames. A held lease keeps one channel
 * buffer busy, so usable capacity is additionally bounded by channel_buffers.
 */
#define DART_CAMERA_MAX_OUTSTANDING_FRAMES 10u

/* Handles are created once at startup. All calls must be serialized. */
typedef struct dart_media dart_media_t;
typedef struct dart_camera dart_camera_t;

/* ABI values owned by the C state machine; mirrored by C++ with static_assert. */
enum {
    DART_CAMERA_STATE_EMPTY = 0,
    DART_CAMERA_STATE_INITIALIZED = 1,
    DART_CAMERA_STATE_STARTED = 2,
    DART_CAMERA_STATE_FAULTED = 3,
};

enum {
    DART_CAMERA_EXPOSURE_FIXED = 0,
    DART_CAMERA_EXPOSURE_AUTO = 1,
};

enum {
    DART_CAMERA_FRAME_SOURCE_PTS_UNVALIDATED = 1u,
};

typedef struct dart_camera_config {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t csi_port;         /* CSI0 / CSI1 / CSI2: 0 / 1 / 2. */
    uint32_t sensor_width;
    uint32_t sensor_height;
    uint32_t sensor_fps;       /* Exact mode required; no silent fallback. */
    uint32_t output_width;     /* 320..3840, multiple of 32; no upscaling. */
    uint32_t output_height;    /* 240..2160, even; no upscaling. */
    uint32_t capture_buffers;  /* 3..10. */
    uint32_t channel_buffers;  /* 3..10. */
    uint32_t max_outstanding_frames; /* 1..channel_buffers and <= MAX_OUTSTANDING.
                                      * Capacity is injected by the caller: it may
                                      * hold this many frames at once. 1 keeps the
                                      * single-frame behaviour. */
    uint32_t exposure_mode;
    uint32_t initial_exposure_us;
    uint32_t mirror;           /* 0: none, 1: horizontal, 2: vertical, 3: both. */
} dart_camera_config_t;

typedef struct dart_camera_caps {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t sensor_width;
    uint32_t sensor_height;
    uint32_t sensor_fps;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t exposure_mode;
} dart_camera_caps_t;

typedef struct dart_camera_frame {
    uint32_t abi_version;
    uint32_t struct_size;
    /* Read-only luma (Y8). DMA writes; C invalidates cache before returning.
     * No pixels are copied. Valid only until a successful release, even though
     * mappings persist. Every row is stride bytes; only width bytes are pixels.
     */
    const uint8_t* data;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t flags;
    uint64_t source_pts;  /* Raw SDK value; timebase is unvalidated. */
    uint64_t received_us; /* CLOCK_MONOTONIC receipt time, not exposure time. */
    uint64_t token;
    const dart_camera_t* owner;
} dart_camera_frame_t;

/* Operations return only a code from camera/error_codes.h; outputs carry data.
 * C never logs or constructs errors. Rollback retains the primary return code;
 * failed rollback is observable as DART_CAMERA_STATE_FAULTED via get_state.
 */
void dart_camera_config_init(dart_camera_config_t* config);
int dart_camera_config_validate(const dart_camera_config_t* config);

/* One media owner per process. All MPP consumers must finish before destroy.
 * This slice reserves DEV0/CHN0 and permits one attached camera. The owner
 * must not coexist with independent raw SDK VB initialization/teardown.
 * Creation/destruction allocate/free only at startup/shutdown.
 */
int dart_media_create(dart_media_t** media);
int dart_media_destroy(dart_media_t* media);

int dart_camera_create(dart_camera_t** camera);
int dart_camera_init(dart_camera_t* camera, dart_media_t* media,
                     const dart_camera_config_t* config);
/* Starts stream, obtains sensor handle, then applies initial fixed exposure.
 * If initial_exposure_us is zero in fixed mode, acquire is blocked until set.
 */
int dart_camera_start(dart_camera_t* camera);
/* Finite timeout, 0=poll, <=1000 ms. Up to max_outstanding_frames frames may be
 * outstanding at once, each carrying a distinct token, and release may happen in
 * any order. Two results are normal negatives rather than errors:
 * DART_CAMERA_E_NO_FRAME (no frame arrived in time) and
 * DART_CAMERA_E_LEASE_EXHAUSTED (every slot is held, so this frame is not taken
 * and is recycled by the driver, i.e. dropped by policy).
 */
int dart_camera_acquire_frame(dart_camera_t* camera, uint32_t timeout_ms,
                             dart_camera_frame_t* frame);
/* A failed SDK release retains the lease, so retry with the same frame. A stale,
 * doubled or foreign token is rejected and changes nothing.
 */
int dart_camera_release_frame(dart_camera_t* camera, const dart_camera_frame_t* frame);
/* Stop/deinit return BUSY while any lease is outstanding. */
int dart_camera_stop(dart_camera_t* camera);
/* Also stops an active stream. Retry after cleanup failure; handle is retained. */
int dart_camera_deinit(dart_camera_t* camera);
/* Only succeeds after deinit. Never silently drops resources. */
int dart_camera_destroy(dart_camera_t* camera);
int dart_camera_get_state(const dart_camera_t* camera, uint32_t* state);
/* Lease accounting for callers applying their own bounded-drop policy:
 * outstanding = leases still held, capacity = config.max_outstanding_frames.
 * Observable diagnostics, not an error channel.
 */
int dart_camera_get_lease_usage(const dart_camera_t* camera, uint32_t* outstanding,
                                uint32_t* capacity);
int dart_camera_get_caps(const dart_camera_t* camera, dart_camera_caps_t* caps);

/* Fixed mode only, after start. Range limits preserve SDK fractional values.
 * C++ owns clamping, inward rounding and microseconds-to-seconds conversion.
 * The seconds writer forwards the supplied value to the SDK without policy.
 */
int dart_camera_get_exposure_range(const dart_camera_t* camera, float* min_us, float* max_us);
int dart_camera_set_exposure_seconds(dart_camera_t* camera, float seconds);

#ifdef __cplusplus
}
#endif
#endif
