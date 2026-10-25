#include "camera/c_api/raw10.h"

#include "camera/c_api/camera.h"

int dart_camera_raw10_gray(
    const uint8_t* raw, size_t raw_bytes, uint32_t width, uint32_t height, uint32_t raw_stride,
    uint32_t layout, uint32_t mirror, uint8_t* gray, size_t gray_bytes) {
    const int packed = layout == DART_CAMERA_RAW10_MIPI || layout == DART_CAMERA_RAW10_PACKED_LSB;
    const uint64_t row_bytes = packed ? (uint64_t)width / 4u * 5u : (uint64_t)width * 2u;
    if (raw == NULL || gray == NULL || width == 0u || height == 0u
        || layout > DART_CAMERA_RAW10_PACKED_LSB || mirror > 3u
        || (packed && width % 4u != 0u) || row_bytes > raw_stride
        || (uint64_t)raw_stride * height > raw_bytes || (uint64_t)width * height > gray_bytes) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = raw + (size_t)y * raw_stride;
        const uint32_t dest_y = mirror & 2u ? height - 1u - y : y;
        uint8_t* out = gray + (size_t)dest_y * width;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t sample;
            if (layout == DART_CAMERA_RAW10_MIPI) {
                const uint8_t* group = row + (size_t)(x / 4u) * 5u;
                sample = ((uint32_t)group[x % 4u] << 2u) | ((group[4] >> ((x % 4u) * 2u)) & 3u);
            } else if (layout == DART_CAMERA_RAW10_PACKED_LSB) {
                const uint8_t* group = row + (size_t)(x / 4u) * 5u;
                const uint32_t bit = (x % 4u) * 10u;
                const uint32_t byte = bit / 8u;
                const uint32_t word = group[byte] | ((uint32_t)group[byte + 1u] << 8u);
                sample = (word >> (bit % 8u)) & 0x3ffu;
            } else {
                const uint32_t word =
                    row[(size_t)x * 2u] | ((uint32_t)row[(size_t)x * 2u + 1u] << 8u);
                const uint32_t unused = layout == DART_CAMERA_RAW10_LE16_LSB ? 0xfc00u : 0x003fu;
                if (word & unused)
                    return DART_CAMERA_E_FRAME_GEOMETRY_MISMATCH;
                sample = layout == DART_CAMERA_RAW10_LE16_LSB ? word : word >> 6u;
            }
            const uint32_t dest_x = mirror & 1u ? width - 1u - x : x;
            out[dest_x] = (uint8_t)(sample >> 2u);
        }
    }
    return DART_CAMERA_OK;
}
