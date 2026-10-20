#ifndef DART_CAMERA_RAW10_H
#define DART_CAMERA_RAW10_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode configured RAW10 packing directly into 0/255 pixels. Padding is
 * skipped; nonzero unused bits in 16-bit layouts reject the entire frame.
 * Input/output must not overlap. A failed output must not be published.
 */
int dart_camera_raw10_binary(
    const uint8_t* raw, size_t raw_bytes, uint32_t width, uint32_t height, uint32_t raw_stride,
    uint32_t layout, uint32_t threshold, uint32_t mirror, uint8_t* binary, size_t binary_bytes);

#ifdef __cplusplus
}
#endif
#endif
