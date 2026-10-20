#include "camera/c_api/lease.h"

#include <stddef.h>

/* Each held lease keeps one channel buffer busy, so the compile-time ceiling can
 * never exceed the buffer ceiling of the pool that backs those frames.
 */
_Static_assert(DART_CAMERA_MAX_OUTSTANDING_FRAMES >= 1u,
               "at least one frame lease slot is required");
_Static_assert(DART_CAMERA_MAX_OUTSTANDING_FRAMES <= DART_CAMERA_MAX_BUFFERS,
               "a lease pool cannot outnumber the channel buffers backing it");

void dart_frame_lease_pool_init(dart_frame_lease_pool_t* pool, uint32_t capacity) {
    if (pool == NULL) return;
    for (uint32_t i = 0; i < DART_CAMERA_MAX_OUTSTANDING_FRAMES; ++i) {
        pool->slots[i].token = 0;
    }
    pool->next_token = 0;
    pool->capacity = capacity > DART_CAMERA_MAX_OUTSTANDING_FRAMES
                         ? DART_CAMERA_MAX_OUTSTANDING_FRAMES
                         : capacity;
    pool->active = 0;
}

int dart_frame_lease_acquire(dart_frame_lease_pool_t* pool, const dart_camera_t* owner,
                             dart_camera_frame_t* frame, uint32_t* slot_out) {
    if (pool == NULL || owner == NULL || frame == NULL || slot_out == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (pool->active >= pool->capacity) {
        return DART_CAMERA_E_LEASE_EXHAUSTED;
    }
    if (pool->next_token >= UINT64_MAX) {
        return DART_CAMERA_E_OUT_OF_RANGE;
    }

    uint32_t slot = DART_CAMERA_MAX_OUTSTANDING_FRAMES;
    for (uint32_t i = 0; i < pool->capacity; ++i) {
        if (pool->slots[i].token == 0) {
            slot = i;
            break;
        }
    }
    if (slot == DART_CAMERA_MAX_OUTSTANDING_FRAMES) {
        /* active < capacity already proves a free slot exists; this is a
         * canary for a pool whose counters were corrupted by the caller.
         */
        return DART_CAMERA_E_BUSY;
    }

    ++pool->next_token;
    pool->slots[slot].token = pool->next_token;
    ++pool->active;
    frame->token = pool->next_token;
    frame->owner = owner;
    *slot_out = slot;
    return DART_CAMERA_OK;
}

int dart_frame_lease_resolve(const dart_frame_lease_pool_t* pool, const dart_camera_t* owner,
                             const dart_camera_frame_t* frame, uint32_t* slot_out) {
    if (pool == NULL || owner == NULL || frame == NULL || slot_out == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (frame->abi_version != DART_CAMERA_ABI_VERSION ||
        frame->struct_size != sizeof(*frame)) {
        return DART_CAMERA_E_LEASE_ABI_MISMATCH;
    }
    if (frame->owner != owner || frame->token == 0) {
        return DART_CAMERA_E_STALE_LEASE;
    }
    for (uint32_t i = 0; i < pool->capacity; ++i) {
        if (pool->slots[i].token == frame->token) {
            *slot_out = i;
            return DART_CAMERA_OK;
        }
    }
    /* Released already, or the caller handed over a token this pool never
     * issued: the frame is not ours to release.
     */
    return DART_CAMERA_E_STALE_LEASE;
}

void dart_frame_lease_commit(dart_frame_lease_pool_t* pool, uint32_t slot) {
    if (pool == NULL || slot >= DART_CAMERA_MAX_OUTSTANDING_FRAMES) return;
    if (pool->slots[slot].token == 0) return;
    pool->slots[slot].token = 0;
    if (pool->active > 0u) --pool->active;
}

uint32_t dart_frame_lease_capacity(const dart_frame_lease_pool_t* pool) {
    return pool == NULL ? 0u : pool->capacity;
}

uint32_t dart_frame_lease_active(const dart_frame_lease_pool_t* pool) {
    return pool == NULL ? 0u : pool->active;
}
