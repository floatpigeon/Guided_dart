#ifndef DART_CAMERA_INTERNAL_H
#define DART_CAMERA_INTERNAL_H

#include <stdint.h>

#include "camera/c_api/lease.h"
#include "camera/k230/media.h"

#include "k_vicap_comm.h"

enum { DART_CAMERA_MAGIC = 0x4443414Du };

struct dart_camera {
    uint32_t magic;
    uint32_t state;
    dart_media_t* media;
    dart_camera_config_t config;
    dart_camera_caps_t caps;
    dart_camera_output_pool_t output_pool;
    int32_t raw_pool_id;
    uint32_t raw_stride;
    uint8_t* binary_frames;
    uint8_t sensor_powered;
    uint8_t sensor_streaming;
    k_vicap_sensor_type sensor_type;
    int32_t sensor_fd;
    uint8_t vicap_initialized;
    uint8_t stream_started;
    uint8_t exposure_configured;
    /* RAW dumps are released immediately after conversion. frame_held tracks
     * only in-progress/failed releases, which must drain before stream stop.
     * Public leases independently retain their per-slot binary CPU buffers.
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

/* Startup-only dump discovery and mapping; no public lease is issued. */
int dart_camera_prepare_frame_mappings(dart_camera_t* camera);

/* Scope the private ioctl adapter to these SDK calls and this camera only. */
int dart_camera_initialize_mcm(dart_camera_t* camera, k_vicap_chn_attr channel);

static inline k_vicap_dump_format dart_camera_dump_format(const dart_camera_t* camera) {
    (void)camera;
    /* MPI dispatch selector only: ONLY_MCM forwards this to MCM dump, without
     * YUV conversion. The DMA format is explicitly configured as RAW10. */
    return VICAP_DUMP_YUV444;
}

static inline uint32_t dart_camera_buffer_count(const dart_camera_t* camera) {
    (void)camera;
    /* The checked kernel has a fixed four-entry MCM ring. buff_num must never
     * exceed that array, irrespective of the number of binary lease slots. */
    return VICAP_MCM_FRAME_COUNT;
}

#endif
