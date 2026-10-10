#define _POSIX_C_SOURCE 200809L
#include "camera.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "exposure_plan.h"
#include "frame_lease.h"

#include <time.h>

#include "k_vb_comm.h"
#include "mpi_sensor_api.h"
#include "mpi_sys_api.h"
#include "mpi_vb_api.h"
#include "mpi_vicap_api.h"

struct dart_media {
    uint32_t magic;
    uint8_t vb_initialized;
    uint8_t camera_attached;
};

struct dart_camera {
    uint32_t magic;
    uint32_t state;
    dart_media_t *media;
    dart_camera_config_t config;
    dart_camera_caps_t caps;
    dart_error_t error;
    int32_t sensor_fd;
    uint8_t vicap_initialized;
    uint8_t stream_started;
    uint8_t frame_held;
    uint8_t exposure_configured;
    dart_frame_lease_t lease;
    k_video_frame_info frame_info;
    int32_t channel_pool;
    uint32_t block_bytes;
    struct {
        k_vb_blk_handle block;
        uint64_t physical;
        void *address;
    } mappings[DART_CAMERA_MAX_BUFFERS];
};

enum { MEDIA_MAGIC = 0x444D4544u, CAMERA_MAGIC = 0x4443414Du };

static void set_error(dart_camera_t *camera, dart_status_t status,
                      uint32_t operation, int32_t native_code) {
    if (camera != NULL) {
        camera->error = (dart_error_t){1u, sizeof(dart_error_t), status,
                                       operation, native_code, 0};
    }
}

static dart_status_t translate(k_s32 native_code) {
    if (native_code == 0) return DART_STATUS_OK;
    if (native_code == K_ERR_VICAP_BUSY) return DART_STATUS_BUSY;
    if (native_code == K_ERR_VICAP_NOMEM) return DART_STATUS_NO_MEMORY;
    if (native_code == K_ERR_VICAP_NOT_SUPPORT) return DART_STATUS_NOT_SUPPORTED;
    return DART_STATUS_HARDWARE_ERROR;
}

static dart_status_t valid_camera(const dart_camera_t *camera) {
    return camera != NULL && camera->magic == CAMERA_MAGIC
        ? DART_STATUS_OK : DART_STATUS_INVALID_ARGUMENT;
}

static dart_status_t monotonic_us(uint64_t *timestamp) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return DART_STATUS_HARDWARE_ERROR;
    *timestamp = (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
    return DART_STATUS_OK;
}

static dart_status_t fail(dart_camera_t *camera, dart_status_t status,
                          uint32_t operation, int32_t native_code) {
    set_error(camera, status, operation, native_code);
    return status;
}

static dart_status_t release_dump(dart_camera_t *camera) {
    if (!camera->frame_held) return DART_STATUS_OK;
    const k_s32 rc = kd_mpi_vicap_dump_release(VICAP_DEV_ID_0, VICAP_CHN_ID_0,
                                               &camera->frame_info);
    if (rc != 0) return fail(camera, DART_STATUS_RELEASE_ERROR, DART_OP_FRAME_RELEASE, rc);
    camera->frame_held = 0;
    return DART_STATUS_OK;
}

/* Enumerate and map our private output pool before streaming. No map/unmap,
 * pixel copies, or application allocations occur in acquire/release.
 */
static dart_status_t prepare_output_pool(dart_camera_t *camera) {
    k_vb_pool_config pool;
    memset(&pool, 0, sizeof(pool));
    pool.blk_size = camera->block_bytes;
    pool.blk_cnt = camera->config.channel_buffers;
    pool.mode = VB_REMAP_MODE_NONE;
    camera->channel_pool = kd_mpi_vb_create_pool(&pool);
    if (camera->channel_pool < 0) {
        return fail(camera, DART_STATUS_NO_MEMORY, DART_OP_POOL_CREATE, camera->channel_pool);
    }
    for (uint32_t i = 0; i < camera->config.channel_buffers; ++i) {
        camera->mappings[i].block = kd_mpi_vb_get_block(camera->channel_pool,
                                                        camera->block_bytes, NULL);
        if (camera->mappings[i].block == VB_INVALID_HANDLE) {
            return fail(camera, DART_STATUS_NO_MEMORY, DART_OP_BLOCK_GET, 0);
        }
        camera->mappings[i].physical = kd_mpi_vb_handle_to_phyaddr(camera->mappings[i].block);
        if (camera->mappings[i].physical == 0) {
            return fail(camera, DART_STATUS_HARDWARE_ERROR, DART_OP_BLOCK_GET, 0);
        }
        camera->mappings[i].address = kd_mpi_sys_mmap_cached(camera->mappings[i].physical,
                                                             camera->block_bytes);
        if (camera->mappings[i].address == NULL) {
            return fail(camera, DART_STATUS_CACHE_SYNC_ERROR, DART_OP_MAP, 0);
        }
    }
    for (uint32_t i = 0; i < camera->config.channel_buffers; ++i) {
        const k_s32 rc = kd_mpi_vb_release_block(camera->mappings[i].block);
        if (rc != 0) return fail(camera, translate(rc), DART_OP_BLOCK_RELEASE, rc);
        camera->mappings[i].block = VB_INVALID_HANDLE;
    }
    return DART_STATUS_OK;
}

dart_status_t dart_media_create(dart_media_t **media, dart_error_t *error) {
    if (media == NULL) return DART_STATUS_INVALID_ARGUMENT;
    *media = NULL;
    dart_media_t *value = (dart_media_t *)calloc(1, sizeof(*value));
    if (value == NULL) return DART_STATUS_NO_MEMORY;
    k_vb_config config;
    memset(&config, 0, sizeof(config));
    config.max_pool_cnt = 64;
    k_s32 rc = kd_mpi_vb_set_config(&config);
    if (rc == 0) rc = kd_mpi_vb_init();
    if (rc != 0) {
        if (error != NULL) *error = (dart_error_t){1u, sizeof(*error),
            translate(rc), DART_OP_MEDIA_INIT, rc, 0};
        free(value);
        return translate(rc);
    }
    value->magic = MEDIA_MAGIC;
    value->vb_initialized = 1;
    *media = value;
    if (error != NULL) *error = (dart_error_t){1u, sizeof(*error),
        DART_STATUS_OK, DART_OP_MEDIA_INIT, 0, 0};
    return DART_STATUS_OK;
}

dart_status_t dart_media_destroy(dart_media_t *media, dart_error_t *error) {
    if (media == NULL || media->magic != MEDIA_MAGIC) return DART_STATUS_INVALID_ARGUMENT;
    if (media->camera_attached) return DART_STATUS_BUSY;
    const k_s32 rc = media->vb_initialized ? kd_mpi_vb_exit() : 0;
    if (rc != 0) {
        if (error != NULL) *error = (dart_error_t){1u, sizeof(*error),
            translate(rc), DART_OP_MEDIA_EXIT, rc, 0};
        return translate(rc);
    }
    media->magic = 0;
    free(media);
    if (error != NULL) *error = (dart_error_t){1u, sizeof(*error),
        DART_STATUS_OK, DART_OP_MEDIA_EXIT, 0, 0};
    return DART_STATUS_OK;
}

dart_status_t dart_camera_create(dart_camera_t **camera) {
    if (camera == NULL) return DART_STATUS_INVALID_ARGUMENT;
    *camera = (dart_camera_t *)calloc(1, sizeof(**camera));
    if (*camera == NULL) return DART_STATUS_NO_MEMORY;
    (*camera)->magic = CAMERA_MAGIC;
    (*camera)->state = DART_CAMERA_EMPTY;
    (*camera)->sensor_fd = -1;
    (*camera)->channel_pool = -1;
    for (uint32_t i = 0; i < DART_CAMERA_MAX_BUFFERS; ++i) {
        (*camera)->mappings[i].block = VB_INVALID_HANDLE;
    }
    set_error(*camera, DART_STATUS_OK, DART_OP_NONE, 0);
    return DART_STATUS_OK;
}

dart_status_t dart_camera_init(dart_camera_t *camera, dart_media_t *media,
                               const dart_camera_config_t *config) {
    if (valid_camera(camera) != DART_STATUS_OK || media == NULL ||
        media->magic != MEDIA_MAGIC || config == NULL) return DART_STATUS_INVALID_ARGUMENT;
    if (camera->state != DART_CAMERA_EMPTY || media->camera_attached) {
        return fail(camera, DART_STATUS_BUSY, DART_OP_CAMERA_INIT, 0);
    }
    dart_status_t status = dart_camera_config_validate(config);
    if (status != DART_STATUS_OK) return fail(camera, status, DART_OP_CAMERA_INIT, 0);
    camera->config = *config;

    k_vicap_probe_config probe;
    memset(&probe, 0, sizeof(probe));
    probe.csi_num = config->csi_port;
    probe.width = config->sensor_width;
    probe.height = config->sensor_height;
    probe.fps = config->sensor_fps;
    k_vicap_sensor_info sensor;
    memset(&sensor, 0, sizeof(sensor));
    k_s32 rc = kd_mpi_sensor_adapt_get(&probe, &sensor);
    if (rc != 0) {
        set_error(camera, translate(rc), DART_OP_SENSOR_PROBE, rc);
        return translate(rc);
    }
    /* The SDK falls back to another mode; reject that ambiguity. */
    if (sensor.width != config->sensor_width || sensor.height != config->sensor_height ||
        sensor.fps != config->sensor_fps || (uint32_t)sensor.csi_num != config->csi_port + 1u ||
        sensor.hdr_mode != VICAP_LINERA_MODE) {
        set_error(camera, DART_STATUS_OUT_OF_RANGE, DART_OP_SENSOR_PROBE, 0);
        return DART_STATUS_OUT_OF_RANGE;
    }
    rc = kd_mpi_vicap_get_sensor_info(sensor.sensor_type, &sensor);
    if (rc != 0) {
        set_error(camera, translate(rc), DART_OP_SENSOR_INFO, rc);
        return translate(rc);
    }

    if (sensor.width != config->sensor_width || sensor.height != config->sensor_height ||
        sensor.fps != config->sensor_fps || (uint32_t)sensor.csi_num != config->csi_port + 1u ||
        sensor.hdr_mode != VICAP_LINERA_MODE) {
        return fail(camera, DART_STATUS_OUT_OF_RANGE, DART_OP_SENSOR_INFO, 0);
    }
    camera->media = media;
    media->camera_attached = 1;
    camera->state = DART_CAMERA_FAULTED;
    camera->block_bytes = (uint32_t)VB_ALIGN_UP(
        (uint64_t)config->output_width * config->output_height * 3u / 2u, 4096u);
    status = prepare_output_pool(camera);
    if (status != DART_STATUS_OK) goto rollback;

    k_vicap_dev_attr dev;
    memset(&dev, 0, sizeof(dev));
    dev.acq_win.width = sensor.width;
    dev.acq_win.height = sensor.height;
    dev.mode = VICAP_WORK_OFFLINE_MODE;
    dev.input_type = VICAP_INPUT_TYPE_SENSOR;
    dev.buffer_num = config->capture_buffers;
    dev.buffer_size = (k_u32)VB_ALIGN_UP((k_u64)sensor.width * sensor.height * 2u, 4096u);
    dev.buffer_pool_id = VB_INVALID_POOLID;
    dev.pipe_ctrl.data = 0xFFFFFFFFu;
    dev.pipe_ctrl.bits.ae_enable = config->exposure_mode == DART_CAMERA_EXPOSURE_AUTO;
    dev.pipe_ctrl.bits.awb_enable = K_TRUE;
    dev.pipe_ctrl.bits.ahdr_enable = K_FALSE;
    dev.pipe_ctrl.bits.dnr3_enable = K_TRUE;
    dev.sensor_info = sensor;
    dev.mirror = (k_vicap_mirror)config->mirror;
    rc = kd_mpi_vicap_set_dev_attr(VICAP_DEV_ID_0, dev);
    if (rc != 0) { fail(camera, translate(rc), DART_OP_DEVICE_CONFIG, rc); goto rollback; }

    k_vicap_chn_attr channel;
    memset(&channel, 0, sizeof(channel));
    channel.out_win.width = config->output_width;
    channel.out_win.height = config->output_height;
    channel.crop_win = dev.acq_win;
    channel.scale_win = channel.out_win;
    channel.scale_enable = K_TRUE;
    channel.chn_enable = K_TRUE;
    channel.pix_format = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    channel.buffer_num = config->channel_buffers;
    channel.buffer_size = (k_u32)VB_ALIGN_UP(
        (k_u64)config->output_width * config->output_height * 3u / 2u, 4096u);
    channel.buffer_pool_id = camera->channel_pool;
    channel.alignment = 12;
    rc = kd_mpi_vicap_set_chn_attr(VICAP_DEV_ID_0, VICAP_CHN_ID_0, channel);
    if (rc != 0) { fail(camera, translate(rc), DART_OP_CHANNEL_CONFIG, rc); goto rollback; }
    /* Treat an attempted SDK init as owning partial resources until deinit. */
    camera->vicap_initialized = 1;
    rc = kd_mpi_vicap_init(VICAP_DEV_ID_0);
    if (rc != 0) { fail(camera, translate(rc), DART_OP_VICAP_INIT, rc); goto rollback; }
    camera->caps = (dart_camera_caps_t){DART_CAMERA_ABI_VERSION, sizeof(camera->caps),
        sensor.width, sensor.height, sensor.fps, config->output_width,
        config->output_height, config->exposure_mode};
    camera->state = DART_CAMERA_INITIALIZED;
    set_error(camera, DART_STATUS_OK, DART_OP_CAMERA_INIT, 0);
    return DART_STATUS_OK;

rollback: {
    dart_error_t primary = camera->error;
    status = dart_camera_deinit(camera);
    if (status != DART_STATUS_OK) {
        primary.cleanup_code = camera->error.native_code != 0
            ? camera->error.native_code : status;
    }
    camera->error = primary;
    return primary.status;
}
}

static dart_status_t apply_exposure(dart_camera_t *camera, uint32_t requested,
                                    dart_exposure_result_t *result) {
    k_sensor_exposure_time_range range;
    memset(&range, 0, sizeof(range));
    k_s32 rc = kd_mpi_sensor_get_exposure_time_range(camera->sensor_fd, &range);
    if (rc != 0) {
        set_error(camera, translate(rc), DART_OP_EXPOSURE_RANGE, rc);
        return translate(rc);
    }
    float seconds = 0.0f;
    dart_status_t status = dart_exposure_plan(requested, range.min_intg_time_us,
                                              range.max_intg_time_us, result, &seconds);
    if (status != DART_STATUS_OK) {
        set_error(camera, status, DART_OP_EXPOSURE_PLAN, 0);
        return status;
    }
    k_sensor_intg_time integration;
    memset(&integration, 0, sizeof(integration));
    integration.intg_time[0] = seconds;
    rc = kd_mpi_sensor_intg_time_set(camera->sensor_fd, integration);
    if (rc != 0) {
        memset(result, 0, sizeof(*result));
        set_error(camera, translate(rc), DART_OP_EXPOSURE_WRITE, rc);
        return translate(rc);
    }
    camera->exposure_configured = 1;
    set_error(camera, DART_STATUS_OK, DART_OP_EXPOSURE_WRITE, 0);
    return DART_STATUS_OK;
}

dart_status_t dart_camera_start(dart_camera_t *camera) {
    if (valid_camera(camera) != DART_STATUS_OK) return DART_STATUS_INVALID_ARGUMENT;
    if (camera->state != DART_CAMERA_INITIALIZED) {
        return fail(camera, DART_STATUS_INVALID_STATE, DART_OP_STREAM_START, 0);
    }
    k_s32 rc = kd_mpi_vicap_start_stream(VICAP_DEV_ID_0);
    if (rc != 0) {
        set_error(camera, translate(rc), DART_OP_STREAM_START, rc);
        return translate(rc);
    }
    camera->stream_started = 1;
    camera->state = DART_CAMERA_STARTED;
    camera->exposure_configured = 0;
    k_vicap_sensor_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.dev_num = VICAP_DEV_ID_0;
    rc = kd_mpi_vicap_get_sensor_fd(&attr);
    dart_status_t status = DART_STATUS_OK;
    if (rc != 0 || attr.sensor_fd < 0) {
        status = fail(camera, DART_STATUS_HARDWARE_ERROR, DART_OP_SENSOR_HANDLE, rc);
        goto rollback_start;
    }
    camera->sensor_fd = attr.sensor_fd;
    if (camera->config.exposure_mode == DART_CAMERA_EXPOSURE_FIXED &&
        camera->config.initial_exposure_us != 0) {
        dart_exposure_result_t result;
        status = apply_exposure(camera, camera->config.initial_exposure_us, &result);
        if (status != DART_STATUS_OK) goto rollback_start;
    } else {
        camera->exposure_configured = camera->config.exposure_mode == DART_CAMERA_EXPOSURE_AUTO;
    }
    set_error(camera, DART_STATUS_OK, DART_OP_STREAM_START, 0);
    return DART_STATUS_OK;

rollback_start: {
    dart_error_t primary = camera->error;
    const dart_status_t cleanup = dart_camera_stop(camera);
    if (cleanup != DART_STATUS_OK) {
        primary.cleanup_code = camera->error.native_code;
        camera->state = DART_CAMERA_FAULTED;
    }
    camera->error = primary;
    return status;
}
}

static dart_status_t abandon_frame(dart_camera_t *camera, dart_status_t status,
                                   uint32_t operation, int32_t native_code) {
    const dart_status_t cleanup = release_dump(camera);
    const int32_t cleanup_code = camera->error.native_code;
    set_error(camera, status, operation, native_code);
    if (cleanup != DART_STATUS_OK) {
        camera->error.cleanup_code = cleanup_code;
        camera->state = DART_CAMERA_FAULTED;
    }
    return status;
}

dart_status_t dart_camera_acquire_frame(dart_camera_t *camera, uint32_t timeout_ms,
                                        dart_camera_frame_t *frame) {
    if (valid_camera(camera) != DART_STATUS_OK || frame == NULL) return DART_STATUS_INVALID_ARGUMENT;
    if (camera->state != DART_CAMERA_STARTED || !camera->exposure_configured) {
        return fail(camera, DART_STATUS_INVALID_STATE, DART_OP_FRAME_ACQUIRE, 0);
    }
    if (camera->frame_held) return fail(camera, DART_STATUS_BUSY, DART_OP_FRAME_ACQUIRE, 0);
    if (timeout_ms > 1000 || camera->lease.generation == UINT64_MAX) {
        return fail(camera, DART_STATUS_OUT_OF_RANGE, DART_OP_FRAME_ACQUIRE, 0);
    }
    memset(frame, 0, sizeof(*frame));
    k_s32 rc = kd_mpi_vicap_dump_frame(VICAP_DEV_ID_0, VICAP_CHN_ID_0,
                                        VICAP_DUMP_YUV, &camera->frame_info, timeout_ms);
    if (rc != 0) {
        const dart_status_t status = rc == K_ERR_VICAP_BUF_EMPTY || rc == K_ERR_VICAP_NOTREADY
            ? DART_STATUS_TIMEOUT : translate(rc);
        return fail(camera, status, DART_OP_FRAME_ACQUIRE, rc);
    }
    camera->frame_held = 1;
    const k_video_frame *source = &camera->frame_info.v_frame;
    const uint64_t length = (uint64_t)source->stride[0] * source->height;
    if (source->phys_addr[0] == 0 || source->width == 0 || source->height == 0 ||
        source->stride[0] < source->width || length > camera->block_bytes ||
        source->width != camera->config.output_width ||
        source->height != camera->config.output_height ||
        source->pixel_format != PIXEL_FORMAT_YUV_SEMIPLANAR_420 ||
        source->compress_mode != COMPRESS_MODE_NONE ||
        camera->frame_info.pool_id != (uint32_t)camera->channel_pool) {
        return abandon_frame(camera, DART_STATUS_HARDWARE_ERROR, DART_OP_FRAME_ACQUIRE, 0);
    }
    void *mapped = NULL;
    for (uint32_t i = 0; i < camera->config.channel_buffers; ++i) {
        const uint64_t physical = camera->mappings[i].physical;
        if (source->phys_addr[0] >= physical) {
            const uint64_t offset = source->phys_addr[0] - physical;
            if (offset <= camera->block_bytes && length <= camera->block_bytes - offset) {
                mapped = (uint8_t *)camera->mappings[i].address + offset;
                break;
            }
        }
    }
    if (mapped == NULL) return abandon_frame(camera, DART_STATUS_HARDWARE_ERROR, DART_OP_MAP, 0);
    rc = kd_mpi_sys_mmz_invalidate_cache(source->phys_addr[0], mapped, (uint32_t)length);
    if (rc != 0) {
        return abandon_frame(camera, DART_STATUS_CACHE_SYNC_ERROR, DART_OP_CACHE_INVALIDATE, rc);
    }
    uint64_t received_us = 0;
    if (monotonic_us(&received_us) != DART_STATUS_OK) {
        return abandon_frame(camera, DART_STATUS_HARDWARE_ERROR, DART_OP_CLOCK, errno);
    }
    frame->abi_version = DART_CAMERA_ABI_VERSION;
    frame->struct_size = sizeof(*frame);
    frame->data = (const uint8_t *)mapped;
    frame->width = source->width;
    frame->height = source->height;
    frame->stride = source->stride[0];
    frame->flags = DART_CAMERA_FRAME_SOURCE_PTS_UNVALIDATED;
    frame->source_pts = source->pts;
    frame->received_us = received_us;
    const dart_status_t status = dart_frame_lease_issue(&camera->lease, camera, frame);
    if (status != DART_STATUS_OK) {
        memset(frame, 0, sizeof(*frame));
        return abandon_frame(camera, status, DART_OP_FRAME_ACQUIRE, 0);
    }
    set_error(camera, DART_STATUS_OK, DART_OP_FRAME_ACQUIRE, 0);
    return DART_STATUS_OK;
}

dart_status_t dart_camera_release_frame(dart_camera_t *camera,
                                        const dart_camera_frame_t *frame) {
    if (valid_camera(camera) != DART_STATUS_OK || frame == NULL) return DART_STATUS_INVALID_ARGUMENT;
    dart_status_t status = dart_frame_lease_validate(&camera->lease, camera, frame);
    if (status != DART_STATUS_OK) return fail(camera, status, DART_OP_FRAME_RELEASE, 0);
    status = release_dump(camera);
    if (status != DART_STATUS_OK) return status;
    dart_frame_lease_commit_release(&camera->lease);
    set_error(camera, DART_STATUS_OK, DART_OP_FRAME_RELEASE, 0);
    return DART_STATUS_OK;
}

dart_status_t dart_camera_stop(dart_camera_t *camera) {
    if (valid_camera(camera) != DART_STATUS_OK) return DART_STATUS_INVALID_ARGUMENT;
    if (camera->lease.active) return fail(camera, DART_STATUS_BUSY, DART_OP_STREAM_STOP, 0);
    if (release_dump(camera) != DART_STATUS_OK) return camera->error.status;
    if (!camera->stream_started) return DART_STATUS_OK;
    const k_s32 rc = kd_mpi_vicap_stop_stream(VICAP_DEV_ID_0);
    if (rc != 0) {
        set_error(camera, translate(rc), DART_OP_STREAM_STOP, rc);
        return translate(rc);
    }
    camera->stream_started = 0;
    if (camera->state != DART_CAMERA_FAULTED) camera->state = DART_CAMERA_INITIALIZED;
    camera->exposure_configured = 0;
    camera->sensor_fd = -1;
    set_error(camera, DART_STATUS_OK, DART_OP_STREAM_STOP, 0);
    return DART_STATUS_OK;
}

dart_status_t dart_camera_deinit(dart_camera_t *camera) {
    if (valid_camera(camera) != DART_STATUS_OK) return DART_STATUS_INVALID_ARGUMENT;
    if (camera->lease.active) return fail(camera, DART_STATUS_BUSY, DART_OP_VICAP_DEINIT, 0);
    if (camera->stream_started || camera->frame_held) {
        dart_status_t status = dart_camera_stop(camera);
        if (status != DART_STATUS_OK) return status;
    }
    if (camera->state != DART_CAMERA_EMPTY) camera->state = DART_CAMERA_FAULTED;
    if (camera->vicap_initialized) {
        const k_s32 rc = kd_mpi_vicap_deinit(VICAP_DEV_ID_0);
        if (rc != 0) {
            set_error(camera, translate(rc), DART_OP_VICAP_DEINIT, rc);
            return translate(rc);
        }
        camera->vicap_initialized = 0;
    }
    for (uint32_t i = 0; i < DART_CAMERA_MAX_BUFFERS; ++i) {
        if (camera->mappings[i].address != NULL) {
            const k_s32 rc = kd_mpi_sys_munmap(camera->mappings[i].address, camera->block_bytes);
            if (rc != 0) return fail(camera, DART_STATUS_CACHE_SYNC_ERROR, DART_OP_UNMAP, rc);
            camera->mappings[i].address = NULL;
        }
        if (camera->mappings[i].block != VB_INVALID_HANDLE) {
            const k_s32 rc = kd_mpi_vb_release_block(camera->mappings[i].block);
            if (rc != 0) return fail(camera, translate(rc), DART_OP_BLOCK_RELEASE, rc);
            camera->mappings[i].block = VB_INVALID_HANDLE;
        }
        camera->mappings[i].physical = 0;
    }
    if (camera->channel_pool >= 0) {
        const k_s32 rc = kd_mpi_vb_destory_pool((uint32_t)camera->channel_pool);
        if (rc != 0) return fail(camera, translate(rc), DART_OP_POOL_DESTROY, rc);
        camera->channel_pool = -1;
    }
    camera->state = DART_CAMERA_EMPTY;
    if (camera->media != NULL) camera->media->camera_attached = 0;
    camera->media = NULL;
    camera->sensor_fd = -1;
    camera->exposure_configured = 0;
    memset(&camera->caps, 0, sizeof(camera->caps));
    set_error(camera, DART_STATUS_OK, DART_OP_VICAP_DEINIT, 0);
    return DART_STATUS_OK;
}

dart_status_t dart_camera_destroy(dart_camera_t *camera) {
    if (valid_camera(camera) != DART_STATUS_OK) return DART_STATUS_INVALID_ARGUMENT;
    if (camera->state != DART_CAMERA_EMPTY || camera->frame_held) return DART_STATUS_BUSY;
    camera->magic = 0;
    free(camera);
    return DART_STATUS_OK;
}

dart_status_t dart_camera_get_state(const dart_camera_t *camera, uint32_t *state) {
    if (valid_camera(camera) != DART_STATUS_OK || state == NULL) return DART_STATUS_INVALID_ARGUMENT;
    *state = camera->state;
    return DART_STATUS_OK;
}

dart_status_t dart_camera_get_caps(const dart_camera_t *camera, dart_camera_caps_t *caps) {
    if (valid_camera(camera) != DART_STATUS_OK || caps == NULL) return DART_STATUS_INVALID_ARGUMENT;
    if (camera->state == DART_CAMERA_EMPTY || camera->state == DART_CAMERA_FAULTED) {
        return DART_STATUS_INVALID_STATE;
    }
    *caps = camera->caps;
    return DART_STATUS_OK;
}

dart_status_t dart_camera_get_error(const dart_camera_t *camera, dart_error_t *error) {
    if (valid_camera(camera) != DART_STATUS_OK || error == NULL) return DART_STATUS_INVALID_ARGUMENT;
    *error = camera->error;
    return DART_STATUS_OK;
}

dart_status_t dart_camera_set_fixed_exposure_us(dart_camera_t *camera, uint32_t exposure_us,
                                                dart_exposure_result_t *result) {
    if (valid_camera(camera) != DART_STATUS_OK) return DART_STATUS_INVALID_ARGUMENT;
    if (result == NULL || exposure_us == 0) {
        return fail(camera, DART_STATUS_INVALID_ARGUMENT, DART_OP_EXPOSURE_WRITE, 0);
    }
    memset(result, 0, sizeof(*result));
    if (camera->state != DART_CAMERA_STARTED || camera->sensor_fd < 0) {
        return fail(camera, DART_STATUS_INVALID_STATE, DART_OP_EXPOSURE_WRITE, 0);
    }
    if (camera->config.exposure_mode != DART_CAMERA_EXPOSURE_FIXED) {
        return fail(camera, DART_STATUS_NOT_SUPPORTED, DART_OP_EXPOSURE_WRITE, 0);
    }
    const dart_status_t status = apply_exposure(camera, exposure_us, result);
    if (status == DART_STATUS_OK) camera->config.initial_exposure_us = result->applied_us;
    return status;
}
