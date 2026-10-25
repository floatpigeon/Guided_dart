#include "camera/c_api/camera.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "camera/k230/internal.h"
#include "k_vb_comm.h"
#include "k_vicap_comm.h"
#include "mpi_sensor_api.h"
#include "mpi_vb_api.h"
#include "mpi_vicap_api.h"

/* These assertions are deliberately kept in the SDK-facing translation unit.
 * The shared error-code header must remain usable by host C and C++ code
 * without pulling in vendor headers.
 */
_Static_assert(
    K_ERR_VICAP_INVALID_DEVID == DART_K230_E_VICAP_INVALID_DEVID,
    "VICAP invalid-device mirror drifted");
_Static_assert(
    K_ERR_VICAP_INVALID_CHNID == DART_K230_E_VICAP_INVALID_CHNID,
    "VICAP invalid-channel mirror drifted");
_Static_assert(
    K_ERR_VICAP_ILLEGAL_PARAM == DART_K230_E_VICAP_ILLEGAL_PARAM,
    "VICAP illegal-param mirror drifted");
_Static_assert(K_ERR_VICAP_EXIST == DART_K230_E_VICAP_EXIST, "VICAP exist mirror drifted");
_Static_assert(K_ERR_VICAP_UNEXIST == DART_K230_E_VICAP_UNEXIST, "VICAP unexist mirror drifted");
_Static_assert(
    K_ERR_VICAP_NULL_PTR == DART_K230_E_VICAP_NULL_PTR, "VICAP null-pointer mirror drifted");
_Static_assert(
    K_ERR_VICAP_NOT_CONFIG == DART_K230_E_VICAP_NOT_CONFIG, "VICAP not-config mirror drifted");
_Static_assert(
    K_ERR_VICAP_NOT_SUPPORT == DART_K230_E_VICAP_NOT_SUPPORT, "VICAP not-support mirror drifted");
_Static_assert(
    K_ERR_VICAP_NOT_PERM == DART_K230_E_VICAP_NOT_PERM, "VICAP not-permitted mirror drifted");
_Static_assert(K_ERR_VICAP_NOMEM == DART_K230_E_VICAP_NOMEM, "VICAP no-memory mirror drifted");
_Static_assert(K_ERR_VICAP_NOBUF == DART_K230_E_VICAP_NOBUF, "VICAP no-buffer mirror drifted");
_Static_assert(
    K_ERR_VICAP_BUF_EMPTY == DART_K230_E_VICAP_BUF_EMPTY, "VICAP empty-buffer mirror drifted");
_Static_assert(
    K_ERR_VICAP_BUF_FULL == DART_K230_E_VICAP_BUF_FULL, "VICAP full-buffer mirror drifted");
_Static_assert(
    K_ERR_VICAP_NOTREADY == DART_K230_E_VICAP_NOTREADY, "VICAP not-ready mirror drifted");
_Static_assert(
    K_ERR_VICAP_BADADDR == DART_K230_E_VICAP_BADADDR, "VICAP bad-address mirror drifted");
_Static_assert(K_ERR_VICAP_BUSY == DART_K230_E_VICAP_BUSY, "VICAP busy mirror drifted");
_Static_assert(
    K_ERR_VICAP_OPT_ALREADY_WRITE == DART_K230_E_VICAP_OPT_ALREADY_WRITE,
    "VICAP option mirror drifted");
_Static_assert(
    K_ERR_VICAP_BUSY <= DART_CAMERA_CODE_VENDOR_MAX,
    "vendor error codes no longer occupy the vendor range");

/* The line exposure writer and its range query live in k230/exposure.c and are
 * declared in c_api/camera.h. Lifecycle only applies the configured initial
 * exposure.
 */

static int valid_camera(const dart_camera_t* camera) {
    return camera != NULL && camera->magic == DART_CAMERA_MAGIC;
}

static int sensor_probe_error(int code) {
    /* kd_mpi_sensor_adapt_get uses small legacy values (1/2) for
     * probe failures; translate those before they enter the site-code ABI. */
    return code > 0 ? DART_CAMERA_E_SENSOR_PROBE_FAILED : code;
}

static int
    exact_sensor_mode(const dart_camera_config_t* config, const k_vicap_sensor_info* sensor) {
    return sensor->width > 0u && sensor->height > 0u
        && (config->sensor_width == 0u || sensor->width == config->sensor_width)
        && (config->sensor_height == 0u || sensor->height == config->sensor_height)
        && (config->sensor_fps == 0u || sensor->fps == config->sensor_fps)
        && (uint32_t)sensor->csi_num == config->csi_port + 1u
        && sensor->hdr_mode == VICAP_LINERA_MODE;
}

static uint32_t raw_capture_block_bytes(uint32_t width, uint32_t height) {
    /* Conservatively reserve three RAW DMA spans, as in the audited probe;
     * the legacy MCM allocator requests width*height*3 even for RAW10. */
    const uint64_t bytes = (uint64_t)width * (uint64_t)height * 2u * 3u;
    return (uint32_t)VB_ALIGN_UP(bytes, 4096u);
}

static int cleanup_init_resources(dart_camera_t* camera) {
    int primary = DART_CAMERA_OK;

    if (camera->stream_started || camera->sensor_powered) {
        const int rc = dart_camera_stop(camera);
        if (rc != DART_CAMERA_OK) {
            primary = rc;
        }
    }

    /* Keep mappings while DMA may still write. Unmap before deinitializing
     * VICAP, then destroy our pool. Failed cleanup retains the retry handle.
     */
    if (primary != DART_CAMERA_OK) {
        camera->state = DART_CAMERA_STATE_FAULTED;
        return primary;
    }

    const int pool_rc = dart_camera_cleanup_output_pool(&camera->output_pool, &camera->state);
    if (pool_rc != DART_CAMERA_OK)
        return pool_rc;

    if (camera->vicap_initialized) {
        const k_s32 rc = kd_mpi_vicap_deinit(VICAP_DEV_ID_0);
        if (rc != 0) {
            camera->state = DART_CAMERA_STATE_FAULTED;
            return rc;
        }
        camera->vicap_initialized = 0;
    }

    if (camera->raw_pool_id >= 0) {
        const int rc = kd_mpi_vb_destory_pool((k_u32)camera->raw_pool_id);
        if (rc != 0) {
            camera->state = DART_CAMERA_STATE_FAULTED;
            return rc;
        }
        camera->raw_pool_id = -1;
    }
    free(camera->gray_frames);
    camera->gray_frames = NULL;

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
        memset(camera->frame_held, 0, sizeof(camera->frame_held));
        dart_frame_lease_pool_init(&camera->leases, 0);
        memset(&camera->config, 0, sizeof(camera->config));
        memset(&camera->caps, 0, sizeof(camera->caps));
    } else {
        camera->state = DART_CAMERA_STATE_FAULTED;
    }
    return primary;
}

int dart_camera_create(dart_camera_t** camera) {
    if (camera == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    *camera = NULL;
    dart_camera_t* value = calloc(1, sizeof(*value));
    if (value == NULL) {
        return DART_CAMERA_E_NO_MEMORY;
    }
    value->magic = DART_CAMERA_MAGIC;
    value->state = DART_CAMERA_STATE_EMPTY;
    value->sensor_fd = -1;
    value->raw_pool_id = -1;
    value->output_pool = (dart_camera_output_pool_t)DART_CAMERA_OUTPUT_POOL_INITIALIZER;
    *camera = value;
    return DART_CAMERA_OK;
}

int dart_camera_init(
    dart_camera_t* camera, dart_media_t* media, const dart_camera_config_t* config) {
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
    /* Capacity is injected by the caller; slots at or above it stay unused. */
    memset(camera->frame_held, 0, sizeof(camera->frame_held));
    dart_frame_lease_pool_init(&camera->leases, config->max_outstanding_frames);
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

    dart_camera_config_t resolved = *config;
    resolved.sensor_width = sensor.width;
    resolved.sensor_height = sensor.height;
    resolved.sensor_fps = sensor.fps;
    if (resolved.output_width == 0u && resolved.output_height == 0u) {
        resolved.output_width = sensor.width;
        resolved.output_height = sensor.height;
    }
    rc = dart_camera_config_validate(&resolved);
    if (rc != DART_CAMERA_OK) {
        (void)cleanup_init_resources(camera);
        return rc;
    }

    rc = kd_mpi_vicap_get_sensor_info(sensor.sensor_type, &sensor);
    if (rc != 0) {
        const int primary = rc;
        (void)cleanup_init_resources(camera);
        return primary;
    }
    if (!exact_sensor_mode(&resolved, &sensor)) {
        const int primary = DART_CAMERA_E_SENSOR_MODE_MISMATCH;
        (void)cleanup_init_resources(camera);
        return primary;
    }

    const int mono =
        sensor.sensor_name != NULL && strncmp(sensor.sensor_name, "sc038hgs_csi", 11u) == 0;
    /* No fallback to ISP, scaling or Bayer processing. This application uses
     * the SC038HGS monochrome mode at its native geometry. */
    if (!mono || sensor.data_type != VICAP_CSI_DATA_TYPE_RAW10 || sensor.width != 640u
        || sensor.height != 480u || resolved.output_width != sensor.width
        || resolved.output_height != sensor.height
        || resolved.exposure_mode != DART_CAMERA_EXPOSURE_FIXED
        || resolved.raw10_layout != DART_CAMERA_RAW10_PACKED_LSB) {
        (void)cleanup_init_resources(camera);
        return DART_CAMERA_E_NOT_SUPPORTED;
    }

    /* All allocation and frame validation below use the resolved geometry. */
    camera->config = resolved;
    config = &camera->config;
    camera->sensor_type = sensor.sensor_type;
    /* Hardware-verified MCM RAW10 is tightly packed: 4 samples / 5 bytes.
     * The conservative SDK DMA span is separate; see k230/mcm.c. */
    camera->raw_stride = sensor.width / 4u * 5u;
    camera->gray_frames =
        malloc((size_t)sensor.width * sensor.height * config->max_outstanding_frames);
    if (camera->gray_frames == NULL) {
        (void)cleanup_init_resources(camera);
        return DART_CAMERA_E_NO_MEMORY;
    }
    k_vb_pool_config pool = {0};
    pool.blk_size = raw_capture_block_bytes(sensor.width, sensor.height);
    pool.blk_cnt = dart_camera_buffer_count(camera);
    pool.mode = VB_REMAP_MODE_NOCACHE;
    camera->raw_pool_id = kd_mpi_vb_create_pool(&pool);
    if (camera->raw_pool_id < 0) {
        const int primary = camera->raw_pool_id;
        (void)cleanup_init_resources(camera);
        return primary;
    }
    camera->output_pool.block_bytes = (uint32_t)pool.blk_size;

    k_vicap_dev_attr dev = {0};
    dev.acq_win.width = sensor.width;
    dev.acq_win.height = sensor.height;
    dev.mode = VICAP_WORK_ONLY_MCM_MODE;
    dev.input_type = VICAP_INPUT_TYPE_SENSOR;
    dev.buffer_num = dart_camera_buffer_count(camera);
    dev.buffer_size = raw_capture_block_bytes(sensor.width, sensor.height);
    dev.buffer_pool_id = camera->raw_pool_id;
    dev.pipe_ctrl.data = 0;
    dev.sensor_info = sensor;
    /* Mirror is applied to the gray CPU buffer, without ISP geometry. */
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
    channel.chn_enable = K_TRUE;
    channel.pix_format = PIXEL_FORMAT_RGB_BAYER_10BPP;
    channel.buffer_num = dart_camera_buffer_count(camera);
    channel.buffer_size = (uint32_t)pool.blk_size;
    channel.buffer_pool_id = camera->raw_pool_id;
    channel.alignment = 12;
    rc = dart_camera_initialize_mcm(camera, channel);
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

int dart_camera_start(dart_camera_t* camera) {
    if (!valid_camera(camera)) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state != DART_CAMERA_STATE_INITIALIZED) {
        return DART_CAMERA_E_INVALID_STATE;
    }

    int rc;
    {
        k_vicap_sensor_attr sensor_attr = {0};
        sensor_attr.dev_num = VICAP_DEV_ID_0;
        sensor_attr.chn_num = VICAP_CHN_ID_0;
        rc = kd_mpi_vicap_get_sensor_fd(&sensor_attr);
        if (rc == 0 && sensor_attr.sensor_fd < 0)
            rc = DART_CAMERA_E_INVALID_STATE;
        if (rc != 0) {
            const int primary = rc;
            if (dart_camera_stop(camera) != DART_CAMERA_OK)
                camera->state = DART_CAMERA_STATE_FAULTED;
            return primary;
        }
        camera->sensor_fd = sensor_attr.sensor_fd;
    }

    /* ONLY_MCM skips sensor initialization. Power/init before receiver start;
     * start the sensor only after the receiver is ready to accept pixels. */
    camera->sensor_powered = 1;
    rc = kd_mpi_sensor_power_set(camera->sensor_fd, K_TRUE);
    if (rc != 0)
        goto start_failed;
    k_sensor_mode mode = {0};
    mode.sensor_type = camera->sensor_type;
    rc = kd_mpi_sensor_init(camera->sensor_fd, mode);
    if (rc != 0)
        goto start_failed;
    k_sensor_test_pattern pattern = {0};
    rc = kd_mpi_sensor_tpg_set(camera->sensor_fd, pattern);
    if (rc != 0)
        goto start_failed;
    rc = kd_mpi_vicap_start_stream(VICAP_DEV_ID_0);
    if (rc != 0)
        goto start_failed;
    camera->stream_started = 1;
    camera->state = DART_CAMERA_STATE_STARTED;
    camera->exposure_configured = 0;
    camera->sensor_streaming = 1;
    rc = kd_mpi_sensor_stream_enable(camera->sensor_fd, K_TRUE);
    if (rc != 0)
        goto start_failed;
    rc = dart_camera_prepare_frame_mappings(camera);
    if (rc != DART_CAMERA_OK)
        goto start_failed;
    if (camera->config.initial_exposure_lines == 0) {
        /* Zero explicitly preserves the sensor's initialized exposure. */
        camera->exposure_configured = 1;
        return DART_CAMERA_OK;
    }

    rc = dart_camera_set_exposure_lines(camera, camera->config.initial_exposure_lines);
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

start_failed:
    if (dart_camera_stop(camera) != DART_CAMERA_OK)
        camera->state = DART_CAMERA_STATE_FAULTED;
    return rc;
}

int dart_camera_stop(dart_camera_t* camera) {
    if (!valid_camera(camera)) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state == DART_CAMERA_STATE_EMPTY)
        return DART_CAMERA_E_INVALID_STATE;
    if (dart_frame_lease_active(&camera->leases) > 0u) {
        return DART_CAMERA_E_BUSY;
    }
    for (uint32_t slot = 0; slot < DART_CAMERA_MAX_OUTSTANDING_FRAMES; ++slot) {
        const int release_rc = dart_camera_release_dump(camera, slot);
        if (release_rc != DART_CAMERA_OK) {
            camera->state = DART_CAMERA_STATE_FAULTED;
            return release_rc;
        }
    }
    if (camera->sensor_streaming) {
        const int rc = kd_mpi_sensor_stream_enable(camera->sensor_fd, K_FALSE);
        if (rc != 0) {
            camera->state = DART_CAMERA_STATE_FAULTED;
            return rc;
        }
        camera->sensor_streaming = 0;
    }
    if (camera->stream_started) {
        const int rc = kd_mpi_vicap_stop_stream(VICAP_DEV_ID_0);
        if (rc != 0) {
            camera->state = DART_CAMERA_STATE_FAULTED;
            return rc;
        }
        camera->stream_started = 0;
    }
    if (camera->sensor_powered) {
        const int rc = kd_mpi_sensor_power_set(camera->sensor_fd, K_FALSE);
        if (rc != 0) {
            camera->state = DART_CAMERA_STATE_FAULTED;
            return rc;
        }
        camera->sensor_powered = 0;
    }
    camera->sensor_fd = -1;
    camera->exposure_configured = 0;
    if (camera->state != DART_CAMERA_STATE_FAULTED) {
        camera->state = DART_CAMERA_STATE_INITIALIZED;
    }
    return DART_CAMERA_OK;
}

int dart_camera_deinit(dart_camera_t* camera) {
    if (!valid_camera(camera)) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state == DART_CAMERA_STATE_EMPTY) {
        return DART_CAMERA_E_INVALID_STATE;
    }
    if (dart_frame_lease_active(&camera->leases) > 0u) {
        return DART_CAMERA_E_BUSY;
    }

    if (dart_camera_held_frames(camera) > 0u && !camera->stream_started) {
        for (uint32_t slot = 0; slot < DART_CAMERA_MAX_OUTSTANDING_FRAMES; ++slot) {
            const int release_rc = dart_camera_release_dump(camera, slot);
            if (release_rc != DART_CAMERA_OK) {
                camera->state = DART_CAMERA_STATE_FAULTED;
                return release_rc;
            }
        }
    }
    if (camera->stream_started || camera->sensor_powered) {
        const int rc = dart_camera_stop(camera);
        if (rc != DART_CAMERA_OK) {
            return rc;
        }
    }
    return cleanup_init_resources(camera);
}

int dart_camera_destroy(dart_camera_t* camera) {
    if (!valid_camera(camera)) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state != DART_CAMERA_STATE_EMPTY || camera->media != NULL
        || camera->vicap_initialized || camera->stream_started || camera->sensor_powered
        || camera->raw_pool_id >= 0 || camera->gray_frames != NULL
        || dart_camera_held_frames(camera) > 0u || dart_frame_lease_active(&camera->leases) > 0u) {
        return DART_CAMERA_E_BUSY;
    }
    camera->magic = 0;
    free(camera);
    return DART_CAMERA_OK;
}

int dart_camera_get_state(const dart_camera_t* camera, uint32_t* state) {
    if (!valid_camera(camera) || state == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    *state = camera->state;
    return DART_CAMERA_OK;
}

int dart_camera_get_lease_usage(
    const dart_camera_t* camera, uint32_t* outstanding, uint32_t* capacity) {
    if (!valid_camera(camera) || outstanding == NULL || capacity == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    *outstanding = dart_frame_lease_active(&camera->leases);
    *capacity = dart_frame_lease_capacity(&camera->leases);
    return DART_CAMERA_OK;
}

int dart_camera_get_caps(const dart_camera_t* camera, dart_camera_caps_t* caps) {
    if (!valid_camera(camera) || caps == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->state != DART_CAMERA_STATE_INITIALIZED
        && camera->state != DART_CAMERA_STATE_STARTED) {
        return DART_CAMERA_E_INVALID_STATE;
    }
    *caps = camera->caps;
    return DART_CAMERA_OK;
}
