#define _POSIX_C_SOURCE 200809L

#include "camera/k230/internal.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "mpi_sys_api.h"
#include "mpi_vb_api.h"
#include "mpi_vicap_api.h"

static int valid_camera(const dart_camera_t* camera) {
    return camera != NULL && camera->magic == DART_CAMERA_MAGIC;
}

int dart_camera_release_dump(dart_camera_t* camera, uint32_t slot) {
    if (!valid_camera(camera) || slot >= DART_CAMERA_MAX_OUTSTANDING_FRAMES) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (!camera->frame_held[slot]) return DART_CAMERA_OK;
    const k_s32 rc = kd_mpi_vicap_dump_release(VICAP_DEV_ID_0, VICAP_CHN_ID_0,
                                               &camera->frame_info[slot]);
    if (rc != 0) return rc;
    camera->frame_held[slot] = 0;
    return DART_CAMERA_OK;
}

uint32_t dart_camera_held_frames(const dart_camera_t* camera) {
    uint32_t held = 0;
    if (camera == NULL) return 0;
    for (uint32_t slot = 0; slot < DART_CAMERA_MAX_OUTSTANDING_FRAMES; ++slot) {
        if (camera->frame_held[slot]) ++held;
    }
    return held;
}

/* Discard a frame whose validation failed. The dump goes back to the driver and
 * the lease slot is freed, so a failed frame can never leak capacity. A dump the
 * SDK refuses to release is retained for stop()/deinit() and marks FAULTED, but
 * its lease slot is freed anyway: the caller's frame struct is cleared, so that
 * token can never be presented again.
 */
static int abandon_frame(dart_camera_t* camera, uint32_t slot, int primary,
                         dart_camera_frame_t* frame) {
    const int cleanup = dart_camera_release_dump(camera, slot);
    if (frame != NULL) memset(frame, 0, sizeof(*frame));
    dart_frame_lease_commit(&camera->leases, slot);
    if (cleanup != DART_CAMERA_OK) camera->state = DART_CAMERA_STATE_FAULTED;
    return primary;
}

static int monotonic_us(uint64_t* value) {
    struct timespec now = {0};
    if (value == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return DART_CAMERA_E_CLOCK_FAILED;
    }
    *value = (uint64_t)now.tv_sec * UINT64_C(1000000) +
             (uint64_t)now.tv_nsec / UINT64_C(1000);
    return DART_CAMERA_OK;
}

static int frame_geometry_valid(const dart_camera_t* camera,
                                const k_video_frame_info* info,
                                uint64_t* length) {
    const k_video_frame* source = &info->v_frame;
    const uint64_t bytes = (uint64_t)source->stride[0] * source->height;
    if (source->phys_addr[0] == 0 || source->width == 0 || source->height == 0 ||
        source->stride[0] < source->width || bytes > camera->output_pool.block_bytes ||
        source->width != camera->config.output_width ||
        source->height != camera->config.output_height ||
        source->pixel_format != PIXEL_FORMAT_YUV_SEMIPLANAR_420 ||
        source->compress_mode != COMPRESS_MODE_NONE) {
        return DART_CAMERA_E_FRAME_GEOMETRY_MISMATCH;
    }
    *length = bytes;
    return DART_CAMERA_OK;
}

static void* find_mapping(const dart_camera_t* camera, uint64_t physical,
                          uint64_t length) {
    for (uint32_t i = 0; i < camera->output_pool.buffer_count; ++i) {
        const uint64_t base = camera->output_pool.mappings[i].physical;
        if (base == 0 || camera->output_pool.mappings[i].address == NULL ||
            physical < base) {
            continue;
        }
        const uint64_t offset = physical - base;
        if (offset <= camera->output_pool.block_bytes &&
            length <= camera->output_pool.block_bytes - offset) {
            return (uint8_t*)camera->output_pool.mappings[i].address + offset;
        }
    }
    return NULL;
}

int dart_camera_prepare_frame_mappings(dart_camera_t* camera) {
    if (!valid_camera(camera) || !camera->stream_started) {
        return DART_CAMERA_E_INVALID_STATE;
    }
    if (camera->output_pool.pool_id >= 0) return DART_CAMERA_OK;

    const uint32_t slot = 0;
    int result = DART_CAMERA_E_NO_FRAME;
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        const k_s32 rc = kd_mpi_vicap_dump_frame(VICAP_DEV_ID_0, VICAP_CHN_ID_0,
            VICAP_DUMP_YUV, &camera->frame_info[slot], 1000u);
        if (rc == 0) {
            camera->frame_held[slot] = 1;
            result = DART_CAMERA_OK;
            break;
        }
        if (rc != K_ERR_VICAP_BUF_EMPTY && rc != K_ERR_VICAP_NOTREADY) return rc;
    }
    if (result != DART_CAMERA_OK) return result;

    const k_video_frame_info* info = &camera->frame_info[slot];
    uint64_t length = 0;
    result = frame_geometry_valid(camera, info, &length);
    if (result == DART_CAMERA_OK) {
        const k_vb_blk_handle block = kd_mpi_vb_phyaddr_to_handle(info->v_frame.phys_addr[0]);
        const k_s32 pool_id = kd_mpi_vb_handle_to_pool_id(block);
        if (block == VB_INVALID_HANDLE || pool_id < 0 || pool_id >= VB_MAX_POOLS) {
            result = DART_CAMERA_E_FRAME_POOL_MISMATCH;
        } else {
            /* The SDK's frame-info pool_id is not reliable on this build. The
             * VB handle obtained from the physical address is authoritative. */
            result = dart_camera_prepare_output_pool(&camera->output_pool,
                (uint32_t)pool_id, camera->output_pool.block_bytes,
                camera->config.channel_buffers, &camera->state);
        }
    }
    if (result == DART_CAMERA_OK &&
        find_mapping(camera, info->v_frame.phys_addr[0], length) == NULL) {
        result = DART_CAMERA_E_FRAME_NOT_MAPPED;
    }
    const int cleanup = dart_camera_release_dump(camera, slot);
    if (cleanup != DART_CAMERA_OK) {
        camera->state = DART_CAMERA_STATE_FAULTED;
        if (result == DART_CAMERA_OK) result = cleanup;
    }
    return result;
}

int dart_camera_acquire_frame(dart_camera_t* camera, uint32_t timeout_ms,
                              dart_camera_frame_t* frame) {
    if (!valid_camera(camera) || frame == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state != DART_CAMERA_STATE_STARTED || !camera->exposure_configured) {
        return DART_CAMERA_E_INVALID_STATE;
    }
    if (timeout_ms > 1000u) return DART_CAMERA_E_OUT_OF_RANGE;

    memset(frame, 0, sizeof(*frame));
    /* Reserve a slot before touching the driver, so a full pipeline never
     * disturbs the stream: when every slot is held the frame is simply not
     * taken, which is the caller's drop signal.
     */
    uint32_t slot = 0;
    int result = dart_frame_lease_acquire(&camera->leases, camera, frame, &slot);
    if (result != DART_CAMERA_OK) return result;

    const k_s32 dump_rc = kd_mpi_vicap_dump_frame(VICAP_DEV_ID_0, VICAP_CHN_ID_0,
                                                  VICAP_DUMP_YUV,
                                                  &camera->frame_info[slot], timeout_ms);
    if (dump_rc != 0) {
        dart_frame_lease_commit(&camera->leases, slot);
        memset(frame, 0, sizeof(*frame));
        if (dump_rc == K_ERR_VICAP_BUF_EMPTY || dump_rc == K_ERR_VICAP_NOTREADY) {
            return DART_CAMERA_E_NO_FRAME;
        }
        return dump_rc;
    }
    camera->frame_held[slot] = 1;

    uint64_t length = 0;
    result = frame_geometry_valid(camera, &camera->frame_info[slot], &length);
    if (result != DART_CAMERA_OK) return abandon_frame(camera, slot, result, frame);
    const k_vb_blk_handle block = kd_mpi_vb_phyaddr_to_handle(
        camera->frame_info[slot].v_frame.phys_addr[0]);
    if (block == VB_INVALID_HANDLE || camera->output_pool.pool_id < 0 ||
        kd_mpi_vb_handle_to_pool_id(block) != camera->output_pool.pool_id) {
        return abandon_frame(camera, slot, DART_CAMERA_E_FRAME_POOL_MISMATCH, frame);
    }
    const k_video_frame* source = &camera->frame_info[slot].v_frame;
    void* mapped = find_mapping(camera, source->phys_addr[0], length);
    if (mapped == NULL) {
        return abandon_frame(camera, slot, DART_CAMERA_E_FRAME_NOT_MAPPED, frame);
    }
    const k_s32 cache_rc = kd_mpi_sys_mmz_invalidate_cache(
        source->phys_addr[0], mapped, (uint32_t)length);
    if (cache_rc != 0) {
        return abandon_frame(camera, slot, DART_CAMERA_E_CACHE_INVALIDATE_FAILED, frame);
    }
    uint64_t received_us = 0;
    result = monotonic_us(&received_us);
    if (result != DART_CAMERA_OK) return abandon_frame(camera, slot, result, frame);

    /* owner and token were stamped into the frame when the slot was reserved. */
    frame->abi_version = DART_CAMERA_ABI_VERSION;
    frame->struct_size = sizeof(*frame);
    frame->data = (const uint8_t*)mapped;
    frame->width = source->width;
    frame->height = source->height;
    frame->stride = source->stride[0];
    frame->flags = DART_CAMERA_FRAME_SOURCE_PTS_UNVALIDATED;
    frame->source_pts = source->pts;
    frame->received_us = received_us;
    return DART_CAMERA_OK;
}

int dart_camera_release_frame(dart_camera_t* camera,
                              const dart_camera_frame_t* frame) {
    if (!valid_camera(camera) || frame == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    uint32_t slot = 0;
    int result = dart_frame_lease_resolve(&camera->leases, camera, frame, &slot);
    if (result != DART_CAMERA_OK) return result;
    /* The lease stays active when the SDK refuses the release, so the same
     * frame can be retried.
     */
    result = dart_camera_release_dump(camera, slot);
    if (result != DART_CAMERA_OK) return result;
    dart_frame_lease_commit(&camera->leases, slot);
    return DART_CAMERA_OK;
}
