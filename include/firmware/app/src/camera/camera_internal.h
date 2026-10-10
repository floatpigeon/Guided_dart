#ifndef DART_CAMERA_INTERNAL_H
#define DART_CAMERA_INTERNAL_H

#include <stdint.h>

#include "camera_media_internal.h"
#include "frame_lease.h"

#include "k_vicap_comm.h"

enum { DART_CAMERA_MAGIC = 0x4443414Du };

struct dart_camera {
    uint32_t magic;
    uint32_t state;
    dart_media_t *media;
    dart_camera_config_t config;
    dart_camera_caps_t caps;
    dart_camera_output_pool_t output_pool;
    int32_t sensor_fd;
    uint8_t vicap_initialized;
    uint8_t stream_started;
    uint8_t frame_held;
    uint8_t exposure_configured;
    dart_frame_lease_t lease;
    k_video_frame_info frame_info;
};

/* Used by lifecycle cleanup when an acquire-side validation failure retained
 * the SDK dump. A failed release keeps frame_held set so the call is retryable.
 */
int dart_camera_release_dump(dart_camera_t *camera);

#endif
