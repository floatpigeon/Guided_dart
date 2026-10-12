#include "camera/c_api/camera.h"

#include <stddef.h>
#include <string.h>

void dart_camera_config_init(dart_camera_config_t *config) {
    if (config == NULL) return;

    memset(config, 0, sizeof(*config));
    config->abi_version = DART_CAMERA_ABI_VERSION;
    config->struct_size = sizeof(*config);
    config->capture_buffers = 4;
    config->channel_buffers = 4;
    /* One outstanding frame unless the caller injects more: each held lease
     * keeps one channel buffer busy, so capacity may never exceed it. */
    config->max_outstanding_frames = 1;
    config->exposure_mode = DART_CAMERA_EXPOSURE_FIXED;
}

int dart_camera_config_validate(const dart_camera_config_t *config) {
    if (config == NULL) return DART_CAMERA_E_INVALID_ARGUMENT;
    if (config->abi_version != DART_CAMERA_ABI_VERSION ||
        config->struct_size != sizeof(*config)) {
        return DART_CAMERA_E_CONFIG_VERSION;
    }

    if (config->csi_port > 2u || config->mirror > 3u ||
        config->sensor_width == 0u || config->sensor_height == 0u ||
        config->sensor_width > 3840u || config->sensor_height > 2160u ||
        config->sensor_fps == 0u || config->sensor_fps > UINT16_MAX ||
        config->output_width < 320u || config->output_width > 3840u ||
        config->output_width % 32u != 0u || config->output_height < 240u ||
        config->output_height > 2160u || config->output_height % 2u != 0u ||
        config->output_width > config->sensor_width ||
        config->output_height > config->sensor_height ||
        config->capture_buffers < 3u ||
        config->capture_buffers > DART_CAMERA_MAX_BUFFERS ||
        config->channel_buffers < 3u ||
        config->channel_buffers > DART_CAMERA_MAX_BUFFERS ||
        config->max_outstanding_frames == 0u ||
        config->max_outstanding_frames > DART_CAMERA_MAX_OUTSTANDING_FRAMES ||
        config->max_outstanding_frames > config->channel_buffers ||
        config->exposure_mode > DART_CAMERA_EXPOSURE_AUTO ||
        (config->exposure_mode == DART_CAMERA_EXPOSURE_AUTO &&
         config->initial_exposure_us != 0u)) {
        return DART_CAMERA_E_CONFIG_INVALID;
    }
    return DART_CAMERA_OK;
}
