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
        if (pool->mappings[i].block_held) {
            const k_s32 rc = kd_mpi_vb_release_block(pool->mappings[i].block);
            if (rc != 0) {
                if (primary == DART_CAMERA_OK) {
                    primary = rc;
                }
                continue;
            }
            pool->mappings[i].block_held = 0;
            pool->mappings[i].block = 0;
        }
        pool->mappings[i].physical = 0;
    }
    if (primary != DART_CAMERA_OK) {
        *state = DART_CAMERA_STATE_FAULTED;
        return primary;
    }
    if (pool->pool_id >= 0) {
        const k_s32 rc = kd_mpi_vb_destory_pool((k_u32)pool->pool_id);
        if (rc != 0) {
            *state = DART_CAMERA_STATE_FAULTED;
            return rc;
        }
    }
    *pool = (dart_camera_output_pool_t)DART_CAMERA_OUTPUT_POOL_INITIALIZER;
    return DART_CAMERA_OK;
}

int dart_camera_prepare_output_pool(
    dart_camera_output_pool_t* pool, uint32_t block_bytes, uint32_t channel_buffers,
    uint32_t* state) {
    if (pool == NULL || state == NULL || block_bytes == 0) {
        return DART_CAMERA_E_INVALID_ARGUMENT;
    }
    if (channel_buffers < 3 || channel_buffers > DART_CAMERA_MAX_BUFFERS) {
        return DART_CAMERA_E_OUT_OF_RANGE;
    }
    if (pool->pool_id >= 0) {
        return DART_CAMERA_E_BUSY;
    }
    k_vb_pool_config config = {0};
    config.blk_size = block_bytes;
    config.blk_cnt = channel_buffers;
    config.mode = VB_REMAP_MODE_NONE;
    const k_s32 pool_id = kd_mpi_vb_create_pool(&config);
    if (pool_id < 0) {
        if (pool_id == (k_s32)VB_INVALID_POOLID) {
            return DART_CAMERA_E_POOL_CREATE_FAILED;
        }
        return pool_id;
    }
    pool->pool_id = pool_id;
    pool->block_bytes = block_bytes;
    pool->buffer_count = channel_buffers;
    int primary = DART_CAMERA_OK;
    /* Hold every block until enumeration is complete so each mapping belongs
     * to a distinct block. Releasing early could return the same block again.
     */
    for (uint32_t i = 0; i < channel_buffers; ++i) {
        const k_vb_blk_handle block = kd_mpi_vb_get_block((k_u32)pool_id, block_bytes, NULL);
        if (block == VB_INVALID_HANDLE) {
            primary = DART_CAMERA_E_BLOCK_EXHAUSTED;
            goto rollback;
        }
        pool->mappings[i].block = block;
        pool->mappings[i].block_held = 1;
        pool->mappings[i].physical = kd_mpi_vb_handle_to_phyaddr(block);
        if (pool->mappings[i].physical == 0) {
            primary = DART_CAMERA_E_MAP_FAILED;
            goto rollback;
        }
        pool->mappings[i].address = kd_mpi_sys_mmap_cached(pool->mappings[i].physical, block_bytes);
        if (pool->mappings[i].address == NULL) {
            primary = DART_CAMERA_E_MAP_FAILED;
            goto rollback;
        }
    }
    for (uint32_t i = 0; i < channel_buffers; ++i) {
        const k_s32 rc = kd_mpi_vb_release_block(pool->mappings[i].block);
        if (rc != 0) {
            primary = rc;
            goto rollback;
        }
        pool->mappings[i].block_held = 0;
        pool->mappings[i].block = 0;
    }
    return DART_CAMERA_OK;

rollback:
    /* Cleanup alone records FAULTED; its code must not replace the primary. */
    (void)dart_camera_cleanup_output_pool(pool, state);
    return primary;
}
