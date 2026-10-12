#ifndef DART_CAMERA_INTERNAL_H
#define DART_CAMERA_INTERNAL_H

#include <stdint.h>

#include "camera/k230/media.h"
#include "camera/c_api/lease.h"

#include "k_vicap_comm.h"

enum { DART_CAMERA_MAGIC = 0x4443414Du };

struct dart_camera {
    uint32_t magic;
    uint32_t state;
    dart_media_t* media;
    dart_camera_config_t config;
    dart_camera_caps_t caps;
    dart_camera_output_pool_t output_pool;
    int32_t sensor_fd;
    uint8_t vicap_initialized;
    uint8_t stream_started;
    uint8_t exposure_configured;
    /* Frame ownership is per slot: while frame_held[i] is set, slot i owns
     * frame_info[i] and the dump must be released before the stream can stop.
     * The lease pool maps a token back to its slot, so release order is free.
     * Slots at or above config.max_outstanding_frames stay unused.
     */
    uint8_t frame_held[DART_CAMERA_MAX_OUTSTANDING_FRAMES];
    k_video_frame_info frame_info[DART_CAMERA_MAX_OUTSTANDING_FRAMES];
    dart_frame_lease_pool_t leases;
};

/* Used by lifecycle cleanup when an acquire-side validation failure retained
 * the SDK dump. A failed release keeps frame_held[slot] set so the call can be
 * retried from stop()/deinit(). An out-of-range slot is an argument error.
 */
int dart_camera_release_dump(dart_camera_t* camera, uint32_t slot);

/* Slots whose dump is still held. Lifecycle teardown may only proceed once this
 * is zero, exactly as it may only proceed with no active lease.
 */
uint32_t dart_camera_held_frames(const dart_camera_t* camera);

#endif
