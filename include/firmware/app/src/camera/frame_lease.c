#include "frame_lease.h"

#include <stddef.h>

int dart_frame_lease_issue(dart_frame_lease_t *lease, const dart_camera_t *owner,
                           dart_camera_frame_t *frame) {
    if (lease == NULL || owner == NULL || frame == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (lease->active) {
        return DART_CAMERA_E_BUSY;
    }
    if (lease->generation == UINT64_MAX) {
        return DART_CAMERA_E_OUT_OF_RANGE;
    }
    ++lease->generation;
    lease->active = 1;
    frame->owner = owner;
    frame->token = lease->generation;
    return DART_CAMERA_OK;
}

int dart_frame_lease_validate(const dart_frame_lease_t *lease,
                              const dart_camera_t *owner,
                              const dart_camera_frame_t *frame) {
    if (lease == NULL || owner == NULL || frame == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (frame->abi_version != DART_CAMERA_ABI_VERSION ||
        frame->struct_size != sizeof(*frame)) {
        return DART_CAMERA_E_LEASE_ABI_MISMATCH;
    }
    if (!lease->active || frame->owner != owner || frame->token == 0 ||
        frame->token != lease->generation) {
        return DART_CAMERA_E_STALE_LEASE;
    }
    return DART_CAMERA_OK;
}

void dart_frame_lease_commit_release(dart_frame_lease_t *lease) {
    if (lease != NULL) {
        lease->active = 0;
    }
}
