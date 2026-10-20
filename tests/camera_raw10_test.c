#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "camera/c_api/camera.h"
#include "camera/c_api/raw10.h"

enum { WIDTH = 1024, HEIGHT = 2, STRIDE = WIDTH * 2 + 16 };

int main(void) {
    uint8_t raw[HEIGHT * STRIDE], binary[WIDTH * HEIGHT];
    for (uint32_t layout = 0; layout <= DART_CAMERA_RAW10_PACKED_LSB; ++layout) {
        /* All 1024 sample values, in both row directions; poison row padding. */
        memset(raw, 0xa5, sizeof(raw));
        for (uint32_t y = 0; y < HEIGHT; ++y) {
            uint8_t* row = raw + y * STRIDE;
            if (layout >= DART_CAMERA_RAW10_MIPI)
                memset(row, 0, WIDTH / 4 * 5);
            for (uint32_t x = 0; x < WIDTH; ++x) {
                const uint32_t sample = y ? WIDTH - 1 - x : x;
                if (layout == DART_CAMERA_RAW10_MIPI) {
                    uint8_t* group = row + x / 4 * 5;
                    group[x % 4] = (uint8_t)(sample >> 2);
                    group[4] |= (uint8_t)((sample & 3) << (2 * (x % 4)));
                } else if (layout == DART_CAMERA_RAW10_PACKED_LSB) {
                    const uint64_t bits = (uint64_t)sample << (10u * (x % 4u));
                    for (unsigned byte = 0; byte < 5; ++byte)
                        row[x / 4 * 5 + byte] |= (uint8_t)(bits >> (byte * 8u));
                } else {
                    const uint32_t word =
                        layout == DART_CAMERA_RAW10_LE16_MSB ? sample << 6 : sample;
                    row[2 * x] = (uint8_t)word;
                    row[2 * x + 1] = (uint8_t)(word >> 8);
                }
            }
        }
        for (uint32_t threshold = 0; threshold <= 255; ++threshold) {
            for (uint32_t mirror = 0; mirror < 4; ++mirror) {
                assert(
                    dart_camera_raw10_binary(
                        raw, sizeof(raw), WIDTH, HEIGHT, STRIDE, layout, threshold, mirror, binary,
                        sizeof(binary))
                    == 0);
                for (uint32_t y = 0; y < HEIGHT; ++y) {
                    for (uint32_t x = 0; x < WIDTH; ++x) {
                        const uint32_t sx = mirror & 1 ? WIDTH - 1 - x : x;
                        const uint32_t sy = mirror & 2 ? HEIGHT - 1 - y : y;
                        const uint32_t sample = sy ? WIDTH - 1 - sx : sx;
                        assert(binary[y * WIDTH + x] == (sample >= threshold * 4 ? 255 : 0));
                    }
                }
            }
        }
        assert(
            dart_camera_raw10_binary(
                raw, sizeof(raw) - 1, WIDTH, HEIGHT, STRIDE, layout, 50, 0, binary, sizeof(binary))
            != 0);
        assert(
            dart_camera_raw10_binary(
                raw, sizeof(raw), WIDTH, HEIGHT, STRIDE, layout, 50, 0, binary, sizeof(binary) - 1)
            != 0);
        assert(
            dart_camera_raw10_binary(
                raw, sizeof(raw), WIDTH, HEIGHT, 1, layout, 50, 0, binary, sizeof(binary))
            != 0);
        if (layout < DART_CAMERA_RAW10_MIPI) {
            /* Wrong alignment must not silently mask bytes into valid samples. */
            raw[layout == DART_CAMERA_RAW10_LE16_LSB ? 1 : 0] |=
                layout == DART_CAMERA_RAW10_LE16_LSB ? 0x80 : 1;
            assert(
                dart_camera_raw10_binary(
                    raw, sizeof(raw), WIDTH, HEIGHT, STRIDE, layout, 50, 0, binary, sizeof(binary))
                == DART_CAMERA_E_FRAME_GEOMETRY_MISMATCH);
        }
    }
    dart_camera_config_t config;
    dart_camera_config_init(&config);
    assert(config.binary_threshold == 50 && config.raw10_layout == DART_CAMERA_RAW10_PACKED_LSB);
    config.binary_threshold = 256;
    assert(dart_camera_config_validate(&config) == DART_CAMERA_E_CONFIG_INVALID);
    config.binary_threshold = 0;
    config.raw10_layout = 4;
    assert(dart_camera_config_validate(&config) == DART_CAMERA_E_CONFIG_INVALID);
    /* First eight real SC038HGS gradient samples: 384,383,...,377. This
     * hardware fixture distinguishes MCM's LSB packing from CSI MIPI RAW10. */
    const uint8_t captured[] = {0x80, 0xfd, 0xe5, 0x57, 0x5f, 0x7c, 0xed, 0xa5, 0x57, 0x5e};
    uint8_t expected[] = {255, 255, 255, 255, 255, 0, 0, 0};
    assert(
        dart_camera_raw10_binary(
            captured, sizeof(captured), 8, 1, 10, DART_CAMERA_RAW10_PACKED_LSB, 95, 0, binary,
            sizeof(binary))
        == 0);
    assert(memcmp(binary, expected, sizeof(expected)) == 0);
    assert(
        dart_camera_raw10_binary(
            captured, sizeof(captured), 7, 1, 10, DART_CAMERA_RAW10_PACKED_LSB, 95, 0, binary,
            sizeof(binary))
        == DART_CAMERA_E_INVALID_ARGUMENT);
    return 0;
}
