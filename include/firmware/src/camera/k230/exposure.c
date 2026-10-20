#include "camera/k230/internal.h"

#include <stddef.h>

#include "mpi_sensor_api.h"

/* SC038HGS timing policy belongs to this backend. No Pixel CLK is needed
 * when exposure is expressed in lines. The SDK's seconds/fps controls are
 * deliberately unused: raw register writes do not update its timing cache.
 */
enum {
    SC038HGS_MIN_EXPOSURE_LINES = 4u,
    SC038HGS_MIN_FRAME_LINES = 600u,
    SC038HGS_MAX_FRAME_LINES = 0xfff0u,
    SC038HGS_EXPOSURE_MARGIN = 11u,
};

static int valid_camera(const dart_camera_t* camera) {
    return camera != NULL && camera->magic == DART_CAMERA_MAGIC;
}

int dart_camera_get_exposure_range(
    const dart_camera_t* camera, uint32_t* min_lines, uint32_t* max_lines) {
    if (!valid_camera(camera) || min_lines == NULL || max_lines == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->sensor_fd < 0 || !camera->sensor_powered
        || camera->state == DART_CAMERA_STATE_FAULTED)
        return DART_CAMERA_E_INVALID_STATE;
    if (camera->config.exposure_mode == DART_CAMERA_EXPOSURE_AUTO) {
        return DART_CAMERA_E_NOT_SUPPORTED;
    }

    *min_lines = SC038HGS_MIN_EXPOSURE_LINES;
    *max_lines = SC038HGS_MAX_FRAME_LINES - SC038HGS_EXPOSURE_MARGIN;
    return DART_CAMERA_OK;
}

static int exposure_fault(dart_camera_t* camera, int code) {
    /* A failed I2C transfer may still have reached the sensor. Stop/init must
     * reset any incomplete group before another exposure or frame is used. */
    camera->state = DART_CAMERA_STATE_FAULTED;
    camera->exposure_configured = 0;
    return code;
}

int dart_camera_set_exposure_lines(dart_camera_t* camera, uint32_t lines) {
    uint32_t min_lines, max_lines;
    int rc = dart_camera_get_exposure_range(camera, &min_lines, &max_lines);
    if (rc != DART_CAMERA_OK)
        return rc;
    if (lines < min_lines || lines > max_lines)
        return DART_CAMERA_E_OUT_OF_RANGE;

    uint32_t frame_lines = lines + SC038HGS_EXPOSURE_MARGIN;
    if (frame_lines < SC038HGS_MIN_FRAME_LINES)
        frame_lines = SC038HGS_MIN_FRAME_LINES;
    const uint32_t raw = lines * 16u;
    k_u32 high = 0;
    rc = kd_mpi_sensor_reg_read(camera->sensor_fd, 0x3e00, &high);
    if (rc != 0)
        return rc;
    const struct {
        k_u32 address;
        k_u32 value;
    } registers[] = {
        {0x320e, frame_lines >> 8},
        {0x320f, frame_lines & 0xffu},
        {0x3e00, (high & 0xf0u) | ((raw >> 16) & 0x0fu)},
        {0x3e01, (raw >> 8) & 0xffu},
        {0x3e02, raw & 0xffu},
    };

    /* Group 0 has capacity for eight registers; exposure and frame length
     * occupy five and must be launched together (SC038HGS manual p. 29). */
    rc = kd_mpi_sensor_reg_write(camera->sensor_fd, 0x3800, 0x00);
    if (rc != 0)
        return exposure_fault(camera, rc);
    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
        rc = kd_mpi_sensor_reg_write(camera->sensor_fd, registers[i].address, registers[i].value);
        if (rc != 0)
            break;
    }
    const int end_rc = kd_mpi_sensor_reg_write(camera->sensor_fd, 0x3800, 0x10);
    if (rc != 0 || end_rc != 0)
        return exposure_fault(camera, rc != 0 ? rc : end_rc);
    rc = kd_mpi_sensor_reg_write(camera->sensor_fd, 0x3800, 0x60);
    if (rc != 0)
        return exposure_fault(camera, rc);

    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
        k_u32 value = 0;
        rc = kd_mpi_sensor_reg_read(camera->sensor_fd, registers[i].address, &value);
        if (rc != 0)
            return exposure_fault(camera, rc);
        if (value != registers[i].value)
            return exposure_fault(camera, DART_CAMERA_E_EXPOSURE_WRITE_FAILED);
    }
    /* Frame timing changed; do not publish the mode table's old rate. */
    camera->caps.sensor_fps = 0;
    camera->exposure_configured = 1;
    return DART_CAMERA_OK;
}
