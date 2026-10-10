#ifndef DART_CAMERA_FRAME_LEASE_H
#define DART_CAMERA_FRAME_LEASE_H

#include <stdint.h>

#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dart_frame_lease {
    uint64_t generation;
    uint8_t active;
} dart_frame_lease_t;

int dart_frame_lease_issue(dart_frame_lease_t *lease, const dart_camera_t *owner,
                           dart_camera_frame_t *frame);
int dart_frame_lease_validate(const dart_frame_lease_t *lease,
                              const dart_camera_t *owner,
                              const dart_camera_frame_t *frame);
void dart_frame_lease_commit_release(dart_frame_lease_t *lease);

#ifdef __cplusplus
}
#endif

#endif
