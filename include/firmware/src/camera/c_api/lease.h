#ifndef DART_CAMERA_FRAME_LEASE_H
#define DART_CAMERA_FRAME_LEASE_H

#include <stdint.h>

#include "camera/c_api/camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded pool of frame leases. One slot per frame the caller may hold at once;
 * the caller injects the capacity through config.max_outstanding_frames and the
 * array is sized by the compile-time ceiling. Tokens come from a single
 * monotonic counter, so a token identifies its slot unambiguously and a released
 * or foreign token can never resolve.
 *
 * Pure C with no SDK dependency: host-compilable, and the only place that
 * decides whether a release is valid.
 */
typedef struct dart_frame_lease_slot {
    uint64_t token; /* 0 = free, nonzero = held by exactly one frame. */
} dart_frame_lease_slot_t;

typedef struct dart_frame_lease_pool {
    dart_frame_lease_slot_t slots[DART_CAMERA_MAX_OUTSTANDING_FRAMES];
    uint64_t next_token;
    uint32_t capacity;
    uint32_t active;
} dart_frame_lease_pool_t;

#define DART_CAMERA_FRAME_LEASE_POOL_INITIALIZER {.next_token = 0, .capacity = 0, .active = 0}

/* Initialize with DART_CAMERA_FRAME_LEASE_POOL_INITIALIZER before first use.
 * A capacity of zero or above the ceiling leaves the pool unusable, and every
 * acquire reports exhaustion.
 */
void dart_frame_lease_pool_init(dart_frame_lease_pool_t* pool, uint32_t capacity);

/* Take a slot for a freshly dumped frame and stamp owner and token into it.
 * Returns DART_CAMERA_E_LEASE_EXHAUSTED when every slot is held (a normal
 * negative result for a bounded pipeline), or DART_CAMERA_E_OUT_OF_RANGE when
 * the token counter is exhausted. slot_out is written only on success.
 */
int dart_frame_lease_acquire(dart_frame_lease_pool_t* pool, const dart_camera_t* owner,
                             dart_camera_frame_t* frame, uint32_t* slot_out);

/* Validate a frame against the pool and report which slot it belongs to.
 * A stale, doubled or foreign token yields DART_CAMERA_E_STALE_LEASE and
 * changes nothing; a mismatched ABI yields DART_CAMERA_E_LEASE_ABI_MISMATCH.
 * Resolving does not free the slot: the caller releases the SDK dump first and
 * then commits.
 */
int dart_frame_lease_resolve(const dart_frame_lease_pool_t* pool, const dart_camera_t* owner,
                             const dart_camera_frame_t* frame, uint32_t* slot_out);

/* Free a resolved slot. Committing an out-of-range or already free slot is a
 * silent no-op, so callers cannot double-count by accident.
 */
void dart_frame_lease_commit(dart_frame_lease_pool_t* pool, uint32_t slot);

uint32_t dart_frame_lease_capacity(const dart_frame_lease_pool_t* pool);
uint32_t dart_frame_lease_active(const dart_frame_lease_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif
