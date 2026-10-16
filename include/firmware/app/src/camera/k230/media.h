#ifndef DART_CAMERA_MEDIA_INTERNAL_H
#define DART_CAMERA_MEDIA_INTERNAL_H

#include "camera/c_api/camera.h"

/* Private C data-plane interface. No SDK types cross this header. Calls remain
 * serialized, as required by camera.h. VICAP owns the output pool; the camera
 * owns only its mappings. Unmap before VICAP deinit, then detach the camera.
 * If media creation cannot roll back VB init, its non-null output is a
 * destroy-only handle even though the return code reports the init failure.
 */
int dart_media_attach_camera(dart_media_t* media, const dart_camera_t* camera);
int dart_media_detach_camera(dart_media_t* media, const dart_camera_t* camera);

typedef struct dart_camera_output_pool {
    int32_t pool_id;
    uint32_t block_bytes;
    uint32_t buffer_count;
    struct {
        uint64_t physical;
        void* address;
    } mappings[DART_CAMERA_MAX_BUFFERS];
} dart_camera_output_pool_t;

#define DART_CAMERA_OUTPUT_POOL_INITIALIZER {.pool_id = -1}

/* Initialize with DART_CAMERA_OUTPUT_POOL_INITIALIZER before first use.
 * Map the SDK-owned pool identified by a startup dump. Preparation rolls back
 * internally and returns only its primary code. Cleanup retains each failed
 * resource for a later explicit retry, and never resets the caller's state;
 * either function sets it to FAULTED when cleanup fails. Cleanup is only safe
 * after the stream and all dumps have stopped, but before VICAP frees the pool.
 */
int dart_camera_prepare_output_pool(
    dart_camera_output_pool_t* pool, uint32_t pool_id, uint32_t block_bytes,
    uint32_t channel_buffers, uint32_t* state);
int dart_camera_cleanup_output_pool(dart_camera_output_pool_t* pool, uint32_t* state);

#endif
