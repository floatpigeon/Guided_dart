/* C boundary between MicroPython and the unchanged C++ RoiDetector. */
#ifndef DART_CANMV_DETECTOR_BRIDGE_H
#define DART_CANMV_DETECTOR_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
namespace dart::canmv {
extern "C" {
#endif

typedef struct dart_tuning_detector dart_tuning_detector;
typedef struct {
    const char *name; /* RoiDetector::Config field path, e.g. light.min_area. */
    double value;
} dart_tuning_parameter;

typedef struct {
    uint32_t x, y, width, height;
} dart_tuning_rect;

typedef struct {
    int valid;
    float x, y;
    dart_tuning_rect bounds;
    uint32_t area;
    float radius, quality;
    int clipped;
    float contrast, snr, stability, brightness_ratio, gap_contrast;
    int stability_checked;
} dart_tuning_target;

typedef struct {
    int kind; /* 0 none, 1 light, 2 armor; same as TargetKind. */
    dart_tuning_target target, light, armor;
    dart_tuning_rect window;
    int full_frame, rescanned;
    uint32_t candidates, missed_frames, run_high_water;
    uint32_t rejected_contrast, rejected_snr, rejected_stability;
    int phase, full_validation;
    uint32_t stability_checks;
} dart_tuning_detection;

/* Codes 1..7 are DetectionError; 100 means unknown/nonrepresentable parameter.
 * On a parameter error, bad_parameter identifies the supplied array element. */
int dart_tuning_detector_create(const dart_tuning_parameter *parameters, size_t count,
                               dart_tuning_detector **out, size_t *bad_parameter);
int dart_tuning_detector_detect(dart_tuning_detector *detector, const uint8_t *pixels,
                               size_t bytes, uint64_t sequence, uint64_t received_us,
                               uint64_t source_pts, uint32_t flags,
                               dart_tuning_detection *out);
void dart_tuning_detector_reset(dart_tuning_detector *detector);
void dart_tuning_detector_destroy(dart_tuning_detector *detector);

#ifdef __cplusplus
}
} // namespace dart::canmv
#endif
#endif
