#include "camera.h"
#include <string.h>

void dart_camera_config_init(dart_camera_config_t *config) {
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->abi_version = DART_CAMERA_ABI_VERSION;
    config->struct_size = sizeof(*config);
    config->capture_buffers = 4;
    config->channel_buffers = 4;
    config->exposure_mode = DART_CAMERA_EXPOSURE_FIXED;
    /* No invented sensor mode or calibrated exposure defaults. */
}

dart_status_t dart_camera_config_validate(const dart_camera_config_t *config) {
    if (config == NULL || config->abi_version != DART_CAMERA_ABI_VERSION ||
        config->struct_size != sizeof(*config) || config->csi_port > 2 ||
        config->mirror > 3 || config->sensor_width == 0 ||
        config->sensor_height == 0 || config->sensor_width > 3840 ||
        config->sensor_height > 2160 || config->sensor_fps == 0 ||
        config->sensor_fps > UINT16_MAX || config->output_width < 320 ||
        config->output_width > 3840 || config->output_width % 32 != 0 ||
        config->output_height < 240 || config->output_height > 2160 ||
        config->output_height % 2 != 0 ||
        config->output_width > config->sensor_width ||
        config->output_height > config->sensor_height ||
        config->capture_buffers < 3 || config->capture_buffers > DART_CAMERA_MAX_BUFFERS ||
        config->channel_buffers < 3 || config->channel_buffers > DART_CAMERA_MAX_BUFFERS ||
        config->exposure_mode > DART_CAMERA_EXPOSURE_AUTO ||
        (config->exposure_mode == DART_CAMERA_EXPOSURE_AUTO && config->initial_exposure_us != 0)) {
        return DART_STATUS_INVALID_ARGUMENT;
    }
    return DART_STATUS_OK;
}
