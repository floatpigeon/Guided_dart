#include "camera/k230/internal.h"

#include <stddef.h>

#include "mpi_sensor_api.h"

static int valid_camera(const dart_camera_t *camera) {
    return camera != NULL && camera->magic == DART_CAMERA_MAGIC;
}

int dart_camera_get_exposure_range(const dart_camera_t *camera, float *min_us,
                                  float *max_us) {
    if (!valid_camera(camera) || min_us == NULL || max_us == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (camera->sensor_fd < 0) return DART_CAMERA_E_INVALID_STATE;
    if (camera->config.exposure_mode == DART_CAMERA_EXPOSURE_AUTO) {
        return DART_CAMERA_E_NOT_SUPPORTED;
    }

    k_sensor_exposure_time_range range = {0};
    const k_s32 rc = kd_mpi_sensor_get_exposure_time_range(camera->sensor_fd,
                                                            &range);
    if (rc != 0) return rc;
    *min_us = range.min_intg_time_us;
    *max_us = range.max_intg_time_us;
    return DART_CAMERA_OK;
}

int dart_camera_set_exposure_seconds(dart_camera_t *camera, float seconds) {
    if (!valid_camera(camera)) return DART_CAMERA_E_INVALID_ARGUMENT;
    if (camera->sensor_fd < 0) return DART_CAMERA_E_INVALID_STATE;
    if (camera->config.exposure_mode == DART_CAMERA_EXPOSURE_AUTO) {
        return DART_CAMERA_E_NOT_SUPPORTED;
    }

    k_sensor_intg_time integration = {0};
    integration.intg_time[0] = seconds;
    const k_s32 rc = kd_mpi_sensor_intg_time_set(camera->sensor_fd, integration);
    if (rc != 0) return rc;
    camera->exposure_configured = 1;
    return DART_CAMERA_OK;
}
