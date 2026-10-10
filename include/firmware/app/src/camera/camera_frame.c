#define _POSIX_C_SOURCE 200809L

#include "camera_internal.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "mpi_sys_api.h"
#include "mpi_vicap_api.h"

static int valid_camera(const dart_camera_t *camera) {
    return camera != NULL && camera->magic == DART_CAMERA_MAGIC;
}

int dart_camera_release_dump(dart_camera_t *camera) {
    if (!valid_camera(camera)) return DART_CAMERA_E_INVALID_ARGUMENT;
    if (!camera->frame_held) return DART_CAMERA_OK;
    const k_s32 rc = kd_mpi_vicap_dump_release(VICAP_DEV_ID_0, VICAP_CHN_ID_0,
                                               &camera->frame_info);
    if (rc != 0) return rc;
    camera->frame_held = 0;
    return DART_CAMERA_OK;
}

static int abandon_frame(dart_camera_t *camera, int primary,
                         dart_camera_frame_t *frame) {
    const int cleanup = dart_camera_release_dump(camera);
    if (frame != NULL) memset(frame, 0, sizeof(*frame));
    if (cleanup != DART_CAMERA_OK) camera->state = DART_CAMERA_STATE_FAULTED;
    return primary;
}

static int monotonic_us(uint64_t *value) {
    struct timespec now = {0};
    if (value == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return DART_CAMERA_E_CLOCK_FAILED;
    }
    *value = (uint64_t)now.tv_sec * UINT64_C(1000000) +
             (uint64_t)now.tv_nsec / UINT64_C(1000);
    return DART_CAMERA_OK;
}

static int frame_geometry_valid(const dart_camera_t *camera,
                                const k_video_frame_info *info,
                                uint64_t *length) {
    const k_video_frame *source = &info->v_frame;
    const uint64_t bytes = (uint64_t)source->stride[0] * source->height;
    if (source->phys_addr[0] == 0 || source->width == 0 || source->height == 0 ||
        source->stride[0] < source->width || bytes > camera->output_pool.block_bytes ||
        source->width != camera->config.output_width ||
        source->height != camera->config.output_height ||
        source->pixel_format != PIXEL_FORMAT_YUV_SEMIPLANAR_420 ||
        source->compress_mode != COMPRESS_MODE_NONE) {
        return DART_CAMERA_E_FRAME_GEOMETRY_MISMATCH;
    }
    if (info->pool_id != (uint32_t)camera->output_pool.pool_id) {
        return DART_CAMERA_E_FRAME_POOL_MISMATCH;
    }
    *length = bytes;
    return DART_CAMERA_OK;
}

static void *find_mapping(const dart_camera_t *camera, uint64_t physical,
                          uint64_t length) {
    for (uint32_t i = 0; i < camera->output_pool.buffer_count; ++i) {
        const uint64_t base = camera->output_pool.mappings[i].physical;
        if (base == 0 || camera->output_pool.mappings[i].address == NULL ||
            physical < base) continue;
        const uint64_t offset = physical - base;
        if (offset <= camera->output_pool.block_bytes &&
            length <= camera->output_pool.block_bytes - offset) {
            return (uint8_t *)camera->output_pool.mappings[i].address + offset;
        }
    }
    return NULL;
}

int dart_camera_acquire_frame(dart_camera_t *camera, uint32_t timeout_ms,
                              dart_camera_frame_t *frame) {
    if (!valid_camera(camera) || frame == NULL) return DART_CAMERA_E_INVALID_ARGUMENT;
    if (camera->state != DART_CAMERA_STATE_STARTED || !camera->exposure_configured) {
        return DART_CAMERA_E_INVALID_STATE;
    }
    if (camera->lease.active || camera->frame_held) return DART_CAMERA_E_BUSY;
    if (timeout_ms > 1000u || camera->lease.generation == UINT64_MAX) {
        return DART_CAMERA_E_OUT_OF_RANGE;
    }

    memset(frame, 0, sizeof(*frame));
    const k_s32 rc = kd_mpi_vicap_dump_frame(VICAP_DEV_ID_0, VICAP_CHN_ID_0,
                                              VICAP_DUMP_YUV, &camera->frame_info,
                                              timeout_ms);
    if (rc != 0) {
        if (rc == K_ERR_VICAP_BUF_EMPTY || rc == K_ERR_VICAP_NOTREADY) {
            return DART_CAMERA_E_NO_FRAME;
        }
        return rc;
    }
    camera->frame_held = 1;

    uint64_t length = 0;
    int result = frame_geometry_valid(camera, &camera->frame_info, &length);
    if (result != DART_CAMERA_OK) return abandon_frame(camera, result, frame);
    const k_video_frame *source = &camera->frame_info.v_frame;
    void *mapped = find_mapping(camera, source->phys_addr[0], length);
    if (mapped == NULL) {
        return abandon_frame(camera, DART_CAMERA_E_FRAME_NOT_MAPPED, frame);
    }
    const k_s32 cache_rc = kd_mpi_sys_mmz_invalidate_cache(
        source->phys_addr[0], mapped, (uint32_t)length);
    if (cache_rc != 0) {
        return abandon_frame(camera, DART_CAMERA_E_CACHE_INVALIDATE_FAILED, frame);
    }
    uint64_t received_us = 0;
    result = monotonic_us(&received_us);
    if (result != DART_CAMERA_OK) return abandon_frame(camera, result, frame);

    frame->abi_version = DART_CAMERA_ABI_VERSION;
    frame->struct_size = sizeof(*frame);
    frame->data = (const uint8_t *)mapped;
    frame->width = source->width;
    frame->height = source->height;
    frame->stride = source->stride[0];
    frame->flags = DART_CAMERA_FRAME_SOURCE_PTS_UNVALIDATED;
    frame->source_pts = source->pts;
    frame->received_us = received_us;
    result = dart_frame_lease_issue(&camera->lease, camera, frame);
    if (result != DART_CAMERA_OK) return abandon_frame(camera, result, frame);
    return DART_CAMERA_OK;
}

int dart_camera_release_frame(dart_camera_t *camera,
                              const dart_camera_frame_t *frame) {
    if (!valid_camera(camera) || frame == NULL) return DART_CAMERA_E_INVALID_ARGUMENT;
    int result = dart_frame_lease_validate(&camera->lease, camera, frame);
    if (result != DART_CAMERA_OK) return result;
    result = dart_camera_release_dump(camera);
    if (result != DART_CAMERA_OK) return result;
    dart_frame_lease_commit_release(&camera->lease);
    return DART_CAMERA_OK;
}
