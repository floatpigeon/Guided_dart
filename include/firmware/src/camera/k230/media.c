#include "camera/k230/media.h"

#include <stdlib.h>

#include "mpi_sys_api.h"
#include "mpi_vb_api.h"

struct dart_media {
    const dart_camera_t* camera;
    uint8_t vb_initialized;
};

/* Includes an owner retained after a failed VB-init rollback. */
static dart_media_t* media_owner;

int dart_media_create(dart_media_t** media) {
    if (media == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (media_owner != NULL) {
        return DART_CAMERA_E_BUSY;
    }
    *media = NULL;
    dart_media_t* value = calloc(1, sizeof(*value));
    if (value == NULL) {
        return DART_CAMERA_E_NO_MEMORY;
    }
    k_vb_config config = {0};
    config.max_pool_cnt = 64;
    k_s32 rc = kd_mpi_vb_set_config(&config);
    if (rc != 0) {
        free(value);
        return rc;
    }
    media_owner = value;
    rc = kd_mpi_vb_init();
    if (rc != 0) {
        /* Init may have acquired partial resources. If exit also fails, return
         * a cleanup handle even on failure; destroy is the explicit retry and
         * no camera may attach to this handle. Preserve the init error.
         */
        if (kd_mpi_vb_exit() == 0) {
            media_owner = NULL;
            free(value);
        } else {
            *media = value;
        }
        return rc;
    }
    value->vb_initialized = 1;
    *media = value;
    return DART_CAMERA_OK;
}

int dart_media_destroy(dart_media_t* media) {
    if (media == NULL || media != media_owner) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (media->camera != NULL) {
        return DART_CAMERA_E_BUSY;
    }
    const k_s32 rc = kd_mpi_vb_exit();
    if (rc != 0) {
        return rc;
    }
    media_owner = NULL;
    free(media);
    return DART_CAMERA_OK;
}

int dart_media_attach_camera(dart_media_t* media, const dart_camera_t* camera) {
    if (media == NULL || media != media_owner || camera == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (!media->vb_initialized) {
        return DART_CAMERA_E_INVALID_STATE;
    }
    if (media->camera != NULL) {
        return DART_CAMERA_E_BUSY;
    }
    media->camera = camera;
    return DART_CAMERA_OK;
}

int dart_media_detach_camera(dart_media_t* media, const dart_camera_t* camera) {
    if (media == NULL || media != media_owner || camera == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (media->camera != camera) {
        return DART_CAMERA_E_INVALID_STATE;
    }
    media->camera = NULL;
    return DART_CAMERA_OK;
}

int dart_camera_cleanup_output_pool(dart_camera_output_pool_t* pool, uint32_t* state) {
    if (pool == NULL || state == NULL) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    int primary = DART_CAMERA_OK;
    for (uint32_t i = 0; i < DART_CAMERA_MAX_BUFFERS; ++i) {
        if (pool->mappings[i].address != NULL) {
            const k_s32 rc = kd_mpi_sys_munmap(pool->mappings[i].address, pool->block_bytes);
            if (rc != 0) {
                if (primary == DART_CAMERA_OK) {
                    primary = rc;
                }
                continue;
            }
            pool->mappings[i].address = NULL;
        }
        pool->mappings[i].physical = 0;
    }
    if (primary != DART_CAMERA_OK) {
        *state = DART_CAMERA_STATE_FAULTED;
        return primary;
    }
    *pool = (dart_camera_output_pool_t)DART_CAMERA_OUTPUT_POOL_INITIALIZER;
    return DART_CAMERA_OK;
}

int dart_camera_prepare_output_pool(
    dart_camera_output_pool_t* pool, uint32_t pool_id, uint32_t block_bytes,
    uint32_t channel_buffers, uint32_t* state) {
    if (pool == NULL || state == NULL || block_bytes == 0 || pool_id >= VB_MAX_POOLS) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (channel_buffers < 3 || channel_buffers > DART_CAMERA_MAX_BUFFERS) {
        return DART_CAMERA_E_OUT_OF_RANGE;
    }
    if (pool->pool_id >= 0) {
        return DART_CAMERA_E_BUSY;
    }
    pool->pool_id = (int32_t)pool_id;
    pool->block_bytes = block_bytes;
    pool->buffer_count = channel_buffers;
    int primary = DART_CAMERA_OK;
    /* k_vb_comm.h defines handles as pool_id:16 | block_id:16. Query existing
     * blocks without acquiring them: VICAP may already hold every block. Check
     * both directions through the public VB API before mapping any address.
     */
    for (uint32_t i = 0; i < channel_buffers; ++i) {
        const k_vb_blk_handle block = (pool_id << 16u) | i;
        if (kd_mpi_vb_handle_to_pool_id(block) != (k_s32)pool_id) {
            primary = DART_CAMERA_E_FRAME_POOL_MISMATCH;
            goto rollback;
        }
        pool->mappings[i].physical = kd_mpi_vb_handle_to_phyaddr(block);
        const uint64_t physical = pool->mappings[i].physical;
        if (physical == 0 || physical > UINT64_MAX - block_bytes ||
            kd_mpi_vb_phyaddr_to_handle(physical) != block) {
            primary = DART_CAMERA_E_MAP_FAILED;
            goto rollback;
        }
        for (uint32_t j = 0; j < i; ++j) {
            const uint64_t other = pool->mappings[j].physical;
            const uint64_t distance = physical > other ? physical - other : other - physical;
            if (distance < block_bytes) {
                primary = DART_CAMERA_E_FRAME_GEOMETRY_MISMATCH;
                goto rollback;
            }
        }
        pool->mappings[i].address = kd_mpi_sys_mmap_cached(physical, block_bytes);
        if (pool->mappings[i].address == NULL) {
            primary = DART_CAMERA_E_MAP_FAILED;
            goto rollback;
        }
    }
    return DART_CAMERA_OK;

rollback:
    /* Cleanup alone records FAULTED; its code must not replace the primary. */
    (void)dart_camera_cleanup_output_pool(pool, state);
    pool->block_bytes = block_bytes;
    return primary;
}
