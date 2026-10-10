#include "frame_lease.h"
#include <stddef.h>

dart_status_t dart_frame_lease_issue(dart_frame_lease_t *lease,
                                    const dart_camera_t *owner, dart_camera_frame_t *frame) {
    if (lease == NULL || owner == NULL || frame == NULL) return DART_STATUS_INVALID_ARGUMENT;
    if (lease->active) return DART_STATUS_BUSY;
    if (lease->generation == UINT64_MAX) return DART_STATUS_OUT_OF_RANGE;
    lease->active = 1;
    frame->owner = owner;
    frame->token = ++lease->generation;
    return DART_STATUS_OK;
}

dart_status_t dart_frame_lease_validate(const dart_frame_lease_t *lease,
                                       const dart_camera_t *owner,
                                       const dart_camera_frame_t *frame) {
    if (lease == NULL || owner == NULL || frame == NULL ||
        frame->abi_version != DART_CAMERA_ABI_VERSION || frame->struct_size != sizeof(*frame)) {
        return DART_STATUS_INVALID_ARGUMENT;
    }
    return lease->active && frame->owner == owner && frame->token != 0 &&
           frame->token == lease->generation ? DART_STATUS_OK : DART_STATUS_STALE;
}

void dart_frame_lease_commit_release(dart_frame_lease_t *lease) {
    if (lease != NULL) lease->active = 0;
}
