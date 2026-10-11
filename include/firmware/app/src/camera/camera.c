#include "camera.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "camera_internal.h"
#include "k_vb_comm.h"
#include "k_vicap_comm.h"
#include "mpi_sensor_api.h"
#include "mpi_vicap_api.h"

/* These assertions are deliberately kept in the SDK-facing translation unit.
 * The shared error-code header must remain usable by host C and C++ code
 * without pulling in vendor headers.
 */
_Static_assert(K_ERR_VICAP_INVALID_DEVID == DART_K230_E_VICAP_INVALID_DEVID,
               "VICAP invalid-device mirror drifted");
_Static_assert(K_ERR_VICAP_INVALID_CHNID == DART_K230_E_VICAP_INVALID_CHNID,
               "VICAP invalid-channel mirror drifted");
_Static_assert(K_ERR_VICAP_ILLEGAL_PARAM == DART_K230_E_VICAP_ILLEGAL_PARAM,
               "VICAP illegal-param mirror drifted");
_Static_assert(K_ERR_VICAP_EXIST == DART_K230_E_VICAP_EXIST,
               "VICAP exist mirror drifted");
_Static_assert(K_ERR_VICAP_UNEXIST == DART_K230_E_VICAP_UNEXIST,
               "VICAP unexist mirror drifted");
_Static_assert(K_ERR_VICAP_NULL_PTR == DART_K230_E_VICAP_NULL_PTR,
               "VICAP null-pointer mirror drifted");
_Static_assert(K_ERR_VICAP_NOT_CONFIG == DART_K230_E_VICAP_NOT_CONFIG,
               "VICAP not-config mirror drifted");
_Static_assert(K_ERR_VICAP_NOT_SUPPORT == DART_K230_E_VICAP_NOT_SUPPORT,
               "VICAP not-support mirror drifted");
_Static_assert(K_ERR_VICAP_NOT_PERM == DART_K230_E_VICAP_NOT_PERM,
               "VICAP not-permitted mirror drifted");
_Static_assert(K_ERR_VICAP_NOMEM == DART_K230_E_VICAP_NOMEM,
               "VICAP no-memory mirror drifted");
_Static_assert(K_ERR_VICAP_NOBUF == DART_K230_E_VICAP_NOBUF,
               "VICAP no-buffer mirror drifted");
_Static_assert(K_ERR_VICAP_BUF_EMPTY == DART_K230_E_VICAP_BUF_EMPTY,
               "VICAP empty-buffer mirror drifted");
_Static_assert(K_ERR_VICAP_BUF_FULL == DART_K230_E_VICAP_BUF_FULL,
               "VICAP full-buffer mirror drifted");
_Static_assert(K_ERR_VICAP_NOTREADY == DART_K230_E_VICAP_NOTREADY,
               "VICAP not-ready mirror drifted");
_Static_assert(K_ERR_VICAP_BADADDR == DART_K230_E_VICAP_BADADDR,
               "VICAP bad-address mirror drifted");
_Static_assert(K_ERR_VICAP_BUSY == DART_K230_E_VICAP_BUSY,
               "VICAP busy mirror drifted");
_Static_assert(K_ERR_VICAP_OPT_ALREADY_WRITE == DART_K230_E_VICAP_OPT_ALREADY_WRITE,
               "VICAP option mirror drifted");
_Static_assert(K_ERR_VICAP_BUSY <= DART_CAMERA_CODE_VENDOR_MAX,
               "vendor error codes no longer occupy the vendor range");

/* T5 owns range handling and the SDK seconds write. T3 only invokes it for
 * the configured initial fixed exposure after the sensor handle is available.
 */
extern int dart_camera_set_exposure_seconds(dart_camera_t *camera, float seconds);

static int valid_camera(const dart_camera_t *camera) {
    return camera != NULL && camera->magic == DART_CAMERA_MAGIC;
}

static int sensor_probe_error(int code) {
    /* kd_mpi_sensor_adapt_get uses small legacy values (1/2) for
     * probe failures; translate those before they enter the site-code ABI. */
    return code > 0 ? DART_CAMERA_E_SENSOR_PROBE_FAILED : code;
}

static int exact_sensor_mode(const dart_camera_config_t *config,
                             const k_vicap_sensor_info *sensor) {
    return sensor->width == config->sensor_width &&
           sensor->height == config->sensor_height &&
           sensor->fps == config->sensor_fps &&
           (uint32_t)sensor->csi_num == config->csi_port + 1u &&
           sensor->hdr_mode == VICAP_LINERA_MODE;
}

static uint32_t yuv420_block_bytes(uint32_t width, uint32_t height) {
    const uint64_t bytes = (uint64_t)width * (uint64_t)height * 3u / 2u;
    return (uint32_t)VB_ALIGN_UP(bytes, 4096u);
}

static uint32_t raw_capture_block_bytes(uint32_t width, uint32_t height) {
    const uint64_t bytes = (uint64_t)width * (uint64_t)height * 2u;
    return (uint32_t)VB_ALIGN_UP(bytes, 4096u);
}

static int cleanup_init_resources(dart_camera_t *camera) {
    int primary = DART_CAMERA_OK;

    if (camera->stream_started) {
        const int rc = dart_camera_stop(camera);
        if (rc != DART_CAMERA_OK) {
            primary = rc;
        }
    }

    /* Do not tear down the pool while VICAP may still own it. A failed stop
     * leaves the stream live and a failed deinit leaves SDK state uncertain;
     * both cases require an explicit retry from the FAULTED handle.
     */
    if (primary != DART_CAMERA_OK) {
        camera->state = DART_CAMERA_STATE_FAULTED;
        return primary;
    }

    if (camera->vicap_initialized) {
        const k_s32 rc = kd_mpi_vicap_deinit(VICAP_DEV_ID_0);
        if (rc != 0) {
            camera->state = DART_CAMERA_STATE_FAULTED;
            return rc;
        }
        camera->vicap_initialized = 0;
    }

    const int pool_rc = dart_camera_cleanup_output_pool(&camera->output_pool,
                                                         &camera->state);
    if (pool_rc != DART_CAMERA_OK && primary == DART_CAMERA_OK) {
        primary = pool_rc;
    }

    /* Keep the media attachment while any resource remains live. This prevents
     * another camera from taking ownership after a partial rollback.
     */
    if (primary == DART_CAMERA_OK && camera->media != NULL) {
        const int rc = dart_media_detach_camera(camera->media, camera);
        if (rc != DART_CAMERA_OK) {
            primary = rc;
            camera->state = DART_CAMERA_STATE_FAULTED;
        } else {
            camera->media = NULL;
        }
    }

    if (primary == DART_CAMERA_OK) {
        camera->state = DART_CAMERA_STATE_EMPTY;
        camera->sensor_fd = -1;
        camera->stream_started = 0;
        camera->exposure_configured = 0;
        memset(&camera->config, 0, sizeof(camera->config));
        memset(&camera->caps, 0, sizeof(camera->caps));
    } else {
        camera->state = DART_CAMERA_STATE_FAULTED;
    }
    return primary;
}

int dart_camera_create(dart_camera_t **camera) {
    if (camera == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    *camera = NULL;
    dart_camera_t *value = calloc(1, sizeof(*value));
    if (value == NULL) {
        return DART_CAMERA_E_NO_MEMORY;
    }
    value->magic = DART_CAMERA_MAGIC;
    value->state = DART_CAMERA_STATE_EMPTY;
    value->sensor_fd = -1;
    value->output_pool = (dart_camera_output_pool_t)DART_CAMERA_OUTPUT_POOL_INITIALIZER;
    *camera = value;
    return DART_CAMERA_OK;
}

int dart_camera_init(dart_camera_t *camera, dart_media_t *media,
                     const dart_camera_config_t *config) {
    if (!valid_camera(camera) || media == NULL || config == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state != DART_CAMERA_STATE_EMPTY) {
        return DART_CAMERA_E_BUSY;
    }

    int rc = dart_camera_config_validate(config);
    if (rc != DART_CAMERA_OK) {
        return rc;
    }

    rc = dart_media_attach_camera(media, camera);
    if (rc != DART_CAMERA_OK) {
        return rc;
    }
    camera->media = media;
    camera->config = *config;
    camera->state = DART_CAMERA_STATE_INITIALIZED;

    k_vicap_probe_config probe = {0};
    /* The probe table uses CSI0/1/2; the returned sensor-info enum is
     * one-based (VICAP_CSI0/1/2), hence the asymmetric comparison below. */
    probe.csi_num = config->csi_port;
    probe.width = config->sensor_width;
    probe.height = config->sensor_height;
    probe.fps = config->sensor_fps;

    k_vicap_sensor_info sensor = {0};
    rc = kd_mpi_sensor_adapt_get(&probe, &sensor);
    if (rc != 0) {
        const int primary = sensor_probe_error(rc);
        (void)cleanup_init_resources(camera);
        return primary;
    }
    if (!exact_sensor_mode(config, &sensor)) {
        const int primary = DART_CAMERA_E_SENSOR_MODE_MISMATCH;
        (void)cleanup_init_resources(camera);
        return primary;
    }

    rc = kd_mpi_vicap_get_sensor_info(sensor.sensor_type, &sensor);
    if (rc != 0) {
        const int primary = rc;
        (void)cleanup_init_resources(camera);
        return primary;
    }
    if (!exact_sensor_mode(config, &sensor)) {
        const int primary = DART_CAMERA_E_SENSOR_MODE_MISMATCH;
        (void)cleanup_init_resources(camera);
        return primary;
    }

    rc = dart_camera_prepare_output_pool(&camera->output_pool,
                                         yuv420_block_bytes(config->output_width,
                                                            config->output_height),
                                         config->channel_buffers, &camera->state);
    if (rc != DART_CAMERA_OK) {
        (void)cleanup_init_resources(camera);
        return rc;
    }

    k_vicap_dev_attr dev = {0};
    dev.acq_win.width = sensor.width;
    dev.acq_win.height = sensor.height;
    dev.mode = VICAP_WORK_OFFLINE_MODE;
    dev.input_type = VICAP_INPUT_TYPE_SENSOR;
    dev.buffer_num = config->capture_buffers;
    dev.buffer_size = raw_capture_block_bytes(sensor.width, sensor.height);
    dev.buffer_pool_id = (k_s32)VB_INVALID_POOLID;
    dev.pipe_ctrl.data = 0xFFFFFFFFu;
    dev.pipe_ctrl.bits.ae_enable = config->exposure_mode == DART_CAMERA_EXPOSURE_AUTO;
    dev.pipe_ctrl.bits.awb_enable = K_TRUE;
    dev.pipe_ctrl.bits.ahdr_enable = K_FALSE;
    dev.pipe_ctrl.bits.dnr3_enable = K_TRUE;
    dev.sensor_info = sensor;
    dev.mirror = (k_vicap_mirror)config->mirror;
    rc = kd_mpi_vicap_set_dev_attr(VICAP_DEV_ID_0, dev);
    if (rc != 0) {
        const int primary = rc;
        (void)cleanup_init_resources(camera);
        return primary;
    }

    k_vicap_chn_attr channel = {0};
    channel.out_win.width = config->output_width;
    channel.out_win.height = config->output_height;
    channel.crop_win = dev.acq_win;
    channel.scale_win = channel.out_win;
    channel.scale_enable = K_TRUE;
    channel.chn_enable = K_TRUE;
    channel.pix_format = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    channel.buffer_num = config->channel_buffers;
    channel.buffer_size = yuv420_block_bytes(config->output_width, config->output_height);
    channel.buffer_pool_id = camera->output_pool.pool_id;
    channel.alignment = 12;
    rc = kd_mpi_vicap_set_chn_attr(VICAP_DEV_ID_0, VICAP_CHN_ID_0, channel);
    if (rc != 0) {
        const int primary = rc;
        (void)cleanup_init_resources(camera);
        return primary;
    }

    /* An unsuccessful init may still leave SDK-owned state. Mark the deinit
     * obligation before making the call so rollback is always attempted.
     */
    camera->vicap_initialized = 1;
    rc = kd_mpi_vicap_init(VICAP_DEV_ID_0);
    if (rc != 0) {
        const int primary = rc;
        (void)cleanup_init_resources(camera);
        return primary;
    }

    camera->caps.abi_version = DART_CAMERA_ABI_VERSION;
    camera->caps.struct_size = sizeof(camera->caps);
    camera->caps.sensor_width = sensor.width;
    camera->caps.sensor_height = sensor.height;
    camera->caps.sensor_fps = sensor.fps;
    camera->caps.output_width = config->output_width;
    camera->caps.output_height = config->output_height;
    camera->caps.exposure_mode = config->exposure_mode;
    return DART_CAMERA_OK;
}

int dart_camera_start(dart_camera_t *camera) {
    if (!valid_camera(camera)) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state != DART_CAMERA_STATE_INITIALIZED) {
        return DART_CAMERA_E_INVALID_STATE;
    }

    int rc = kd_mpi_vicap_start_stream(VICAP_DEV_ID_0);
    if (rc != 0) {
        return rc;
    }
    camera->stream_started = 1;
    camera->state = DART_CAMERA_STATE_STARTED;
    camera->exposure_configured = 0;

    k_vicap_sensor_attr sensor_attr = {0};
    sensor_attr.dev_num = VICAP_DEV_ID_0;
    sensor_attr.chn_num = VICAP_CHN_ID_0;
    rc = kd_mpi_vicap_get_sensor_fd(&sensor_attr);
    if (rc != 0) {
        const int primary = rc;
        const int cleanup = dart_camera_stop(camera);
        if (cleanup != DART_CAMERA_OK) {
            camera->state = DART_CAMERA_STATE_FAULTED;
        }
        return primary;
    }
    if (sensor_attr.sensor_fd < 0) {
        const int primary = DART_CAMERA_E_INVALID_STATE;
        const int cleanup = dart_camera_stop(camera);
        if (cleanup != DART_CAMERA_OK) {
            camera->state = DART_CAMERA_STATE_FAULTED;
        }
        return primary;
    }
    camera->sensor_fd = sensor_attr.sensor_fd;

    if (camera->config.exposure_mode == DART_CAMERA_EXPOSURE_AUTO) {
        camera->exposure_configured = 1;
        return DART_CAMERA_OK;
    }
    if (camera->config.initial_exposure_us == 0) {
        return DART_CAMERA_OK;
    }

    rc = dart_camera_set_exposure_seconds(
        camera, (float)camera->config.initial_exposure_us / 1000000.0f);
    if (rc != DART_CAMERA_OK) {
        const int primary = rc;
        const int cleanup = dart_camera_stop(camera);
        if (cleanup != DART_CAMERA_OK) {
            camera->state = DART_CAMERA_STATE_FAULTED;
        }
        return primary;
    }
    camera->exposure_configured = 1;
    return DART_CAMERA_OK;
}

int dart_camera_stop(dart_camera_t *camera) {
    if (!valid_camera(camera)) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->lease.active) {
        return DART_CAMERA_E_BUSY;
    }
    if (camera->frame_held) {
        const int release_rc = dart_camera_release_dump(camera);
        if (release_rc != DART_CAMERA_OK) {
            camera->state = DART_CAMERA_STATE_FAULTED;
            return release_rc;
        }
    }
    if (!camera->stream_started) {
        if (camera->state == DART_CAMERA_STATE_EMPTY) {
            return DART_CAMERA_E_INVALID_STATE;
        }
        return DART_CAMERA_OK;
    }

    const int rc = kd_mpi_vicap_stop_stream(VICAP_DEV_ID_0);
    if (rc != 0) {
        camera->state = DART_CAMERA_STATE_FAULTED;
        return rc;
    }
    camera->stream_started = 0;
    camera->sensor_fd = -1;
    camera->exposure_configured = 0;
    if (camera->state != DART_CAMERA_STATE_FAULTED) {
        camera->state = DART_CAMERA_STATE_INITIALIZED;
    }
    return DART_CAMERA_OK;
}

int dart_camera_deinit(dart_camera_t *camera) {
    if (!valid_camera(camera)) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state == DART_CAMERA_STATE_EMPTY) {
        return DART_CAMERA_E_INVALID_STATE;
    }
    if (camera->lease.active) {
        return DART_CAMERA_E_BUSY;
    }

    if (camera->frame_held && !camera->stream_started) {
        const int release_rc = dart_camera_release_dump(camera);
        if (release_rc != DART_CAMERA_OK) {
            camera->state = DART_CAMERA_STATE_FAULTED;
            return release_rc;
        }
    }
    if (camera->stream_started) {
        const int rc = dart_camera_stop(camera);
        if (rc != DART_CAMERA_OK) {
            return rc;
        }
    }
    return cleanup_init_resources(camera);
}

int dart_camera_destroy(dart_camera_t *camera) {
    if (!valid_camera(camera)) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state != DART_CAMERA_STATE_EMPTY || camera->media != NULL ||
        camera->vicap_initialized || camera->stream_started ||
        camera->frame_held || camera->lease.active) {
        return DART_CAMERA_E_BUSY;
    }
    camera->magic = 0;
    free(camera);
    return DART_CAMERA_OK;
}

int dart_camera_get_state(const dart_camera_t *camera, uint32_t *state) {
    if (!valid_camera(camera) || state == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    *state = camera->state;
    return DART_CAMERA_OK;
}

int dart_camera_get_caps(const dart_camera_t *camera, dart_camera_caps_t *caps) {
    if (!valid_camera(camera) || caps == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state != DART_CAMERA_STATE_INITIALIZED &&
        camera->state != DART_CAMERA_STATE_STARTED) {
        return DART_CAMERA_E_INVALID_STATE;
    }
    *caps = camera->caps;
    return DART_CAMERA_OK;
}
