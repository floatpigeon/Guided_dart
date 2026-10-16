#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "camera/k230/internal.h"
#include "mpi_sensor_api.h"
#include "mpi_sys_api.h"
#include "mpi_vb_api.h"
#include "mpi_vicap_api.h"

enum { POOL_ID = 7, BUFFERS = 4, WIDTH = 320, HEIGHT = 240,
       BLOCK_BYTES = 118784, FAKE_FAILURE = -900 };
static uint8_t pixels[BUFFERS][BLOCK_BYTES];
static k_vicap_chn_attr channel;
static uint32_t map_calls, unmap_calls, live_maps, dump_calls, dumps_held, next_block;
static uint32_t deinit_calls, invalidate_calls, fail_map_call;
static int wrong_pool, bad_address, bad_stride, fail_release, fail_unmap, fail_deinit;
static int vb_live, vicap_live, streaming, not_ready_count;

static uint64_t physical(uint32_t index) {
    return UINT64_C(0x10000000) + (uint64_t)index * BLOCK_BYTES;
}

static void reset_fake(void) {
    assert(!vb_live && !vicap_live && !streaming && !live_maps && !dumps_held);
    channel = (k_vicap_chn_attr){0};
    map_calls = unmap_calls = dump_calls = next_block = 0;
    deinit_calls = invalidate_calls = fail_map_call = 0;
    wrong_pool = bad_address = bad_stride = fail_release = fail_unmap = fail_deinit = 0;
    not_ready_count = 0;
}

k_s32 kd_mpi_vb_set_config(const k_vb_config* config) {
    assert(!vb_live && config->comm_pool[0].blk_cnt == 0);
    return 0;
}
k_s32 kd_mpi_vb_init(void) { vb_live = 1; return 0; }
k_s32 kd_mpi_vb_exit(void) {
    assert(!vicap_live && !live_maps && !dumps_held);
    vb_live = 0;
    return 0;
}
k_s32 kd_mpi_vb_handle_to_pool_id(k_vb_blk_handle block) {
    return vicap_live && (block >> 16u) == POOL_ID && (block & 0xffffu) < BUFFERS
        ? POOL_ID : (k_s32)VB_INVALID_POOLID;
}
k_u64 kd_mpi_vb_handle_to_phyaddr(k_vb_blk_handle block) {
    return kd_mpi_vb_handle_to_pool_id(block) == POOL_ID
        ? physical(block & 0xffffu) : 0;
}
k_vb_blk_handle kd_mpi_vb_phyaddr_to_handle(k_u64 address) {
    for (uint32_t i = 0; i < BUFFERS && vicap_live; ++i) {
        if (address == physical(i)) {
            return ((wrong_pool ? POOL_ID + 1u : POOL_ID) << 16u) | i;
        }
    }
    return VB_INVALID_HANDLE;
}
/* VICAP owns every block. The camera must neither allocate another pool nor
 * acquire/release VB blocks while mapping the existing pool. */
k_s32 kd_mpi_vb_create_pool(k_vb_pool_config* config) {
    (void)config; assert(0); return VB_INVALID_POOLID;
}
k_s32 kd_mpi_vb_destory_pool(k_u32 pool) {
    (void)pool; assert(0); return FAKE_FAILURE;
}
k_vb_blk_handle kd_mpi_vb_get_block(k_u32 pool, k_u64 bytes, const k_char* name) {
    (void)pool; (void)bytes; (void)name; assert(0); return VB_INVALID_HANDLE;
}
k_s32 kd_mpi_vb_release_block(k_vb_blk_handle block) {
    (void)block; assert(0); return FAKE_FAILURE;
}
void* kd_mpi_sys_mmap_cached(k_u64 address, k_u32 size) {
    assert(vicap_live && size == BLOCK_BYTES);
    ++map_calls;
    if (map_calls == fail_map_call) return NULL;
    for (uint32_t i = 0; i < BUFFERS; ++i) {
        if (address == physical(i)) { ++live_maps; return pixels[i]; }
    }
    assert(0); return NULL;
}
k_s32 kd_mpi_sys_munmap(void* address, k_u32 size) {
    assert(vicap_live && size == BLOCK_BYTES && address != NULL && live_maps > 0);
    ++unmap_calls;
    if (fail_unmap) return FAKE_FAILURE;
    --live_maps;
    return 0;
}
k_s32 kd_mpi_sys_mmz_invalidate_cache(k_u64 address, void* mapped, k_u32 size) {
    const uint32_t index = (uint32_t)((address - physical(0)) / BLOCK_BYTES);
    assert(index < BUFFERS && mapped == pixels[index] && size == WIDTH * HEIGHT);
    ++invalidate_calls;
    return 0;
}
k_s32 kd_mpi_sensor_adapt_get(k_vicap_probe_config* probe, k_vicap_sensor_info* info) {
    *info = (k_vicap_sensor_info){0};
    info->width = probe->width; info->height = probe->height; info->fps = probe->fps;
    info->csi_num = probe->csi_num + 1u; info->hdr_mode = VICAP_LINERA_MODE;
    return 0;
}
k_s32 kd_mpi_vicap_get_sensor_info(k_vicap_sensor_type type, k_vicap_sensor_info* info) {
    (void)type; (void)info; return 0;
}
k_s32 kd_mpi_vicap_set_dev_attr(k_vicap_dev dev, k_vicap_dev_attr attr) {
    assert(dev == VICAP_DEV_ID_0 && attr.acq_win.width == WIDTH); return 0;
}
k_s32 kd_mpi_vicap_set_chn_attr(k_vicap_dev dev, k_vicap_chn chn, k_vicap_chn_attr attr) {
    assert(dev == VICAP_DEV_ID_0 && chn == VICAP_CHN_ID_0);
    channel = attr;
    channel.buffer_pool_id = (k_s32)VB_INVALID_POOLID;
    channel.buffer_size = BLOCK_BYTES;
    return 0;
}
k_s32 kd_mpi_vicap_get_chn_attr(k_vicap_dev dev, k_vicap_chn chn, k_vicap_chn_attr* attr) {
    (void)dev; (void)chn; *attr = channel; return 0;
}
k_s32 kd_mpi_vicap_init(k_vicap_dev dev) {
    (void)dev; assert(vb_live); vicap_live = 1; return 0;
}
k_s32 kd_mpi_vicap_deinit(k_vicap_dev dev) {
    (void)dev; assert(!streaming && !dumps_held && !live_maps);
    ++deinit_calls;
    if (fail_deinit) return FAKE_FAILURE;
    vicap_live = 0; return 0;
}
k_s32 kd_mpi_vicap_start_stream(k_vicap_dev dev) {
    (void)dev; assert(vicap_live); streaming = 1; return 0;
}
k_s32 kd_mpi_vicap_stop_stream(k_vicap_dev dev) {
    (void)dev; assert(!dumps_held); streaming = 0; return 0;
}
k_s32 kd_mpi_vicap_get_sensor_fd(k_vicap_sensor_attr* attr) {
    attr->sensor_fd = 9; return 0;
}
k_s32 kd_mpi_vicap_dump_frame(k_vicap_dev dev, k_vicap_chn chn,
                             k_vicap_dump_format format, k_video_frame_info* info,
                             k_u32 timeout) {
    (void)dev; (void)chn; (void)format; (void)timeout;
    assert(streaming); ++dump_calls;
    if (not_ready_count > 0) { --not_ready_count; return K_ERR_VICAP_NOTREADY; }
    *info = (k_video_frame_info){0};
    info->pool_id = wrong_pool ? POOL_ID + 1 : POOL_ID;
    info->v_frame.width = WIDTH; info->v_frame.height = HEIGHT;
    info->v_frame.stride[0] = bad_stride ? WIDTH - 1 : WIDTH;
    info->v_frame.pixel_format = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    info->v_frame.compress_mode = COMPRESS_MODE_NONE;
    info->v_frame.phys_addr[0] = bad_address ? UINT64_C(0x90000000)
        : physical(next_block++ % BUFFERS);
    ++dumps_held;
    return 0;
}
k_s32 kd_mpi_vicap_dump_release(k_vicap_dev dev, k_vicap_chn chn,
                               const k_video_frame_info* info) {
    (void)dev; (void)chn; (void)info; assert(dumps_held);
    if (fail_release) return FAKE_FAILURE;
    --dumps_held; return 0;
}
int dart_camera_set_exposure_seconds(dart_camera_t* camera, float seconds) {
    (void)camera; (void)seconds; return 0;
}

static dart_camera_t* initialize(dart_media_t** media) {
    dart_camera_config_t config;
    dart_camera_config_init(&config);
    config.sensor_width = config.output_width = WIDTH;
    config.sensor_height = config.output_height = HEIGHT;
    config.sensor_fps = 30; config.csi_port = 2;
    config.max_outstanding_frames = 2;
    config.exposure_mode = DART_CAMERA_EXPOSURE_AUTO;
    dart_camera_t* camera = NULL;
    assert(dart_media_create(media) == 0);
    assert(dart_camera_create(&camera) == 0);
    assert(dart_camera_init(camera, *media, &config) == 0);
    assert(map_calls == 0 && channel.buffer_pool_id == (k_s32)VB_INVALID_POOLID);
    return camera;
}

static void destroy(dart_camera_t* camera, dart_media_t* media) {
    assert(dart_camera_deinit(camera) == 0);
    assert(dart_camera_destroy(camera) == 0);
    assert(dart_media_destroy(media) == 0);
}

int main(void) {
    dart_media_t* media = NULL;
    reset_fake();
    dart_camera_t* camera = initialize(&media);
    not_ready_count = 1;
    assert(dart_camera_start(camera) == 0);
    assert(map_calls == BUFFERS && dumps_held == 0 && dump_calls == 2);
    assert(camera->output_pool.pool_id == POOL_ID);
    for (uint32_t i = 0; i < 8; ++i) {
        dart_camera_frame_t frame;
        assert(dart_camera_acquire_frame(camera, 20, &frame) == 0);
        assert(frame.data != NULL && frame.width == WIDTH && frame.stride == WIDTH);
        assert(dart_camera_release_frame(camera, &frame) == 0);
    }
    assert(invalidate_calls == 8 && map_calls == BUFFERS && unmap_calls == 0);
    dart_camera_frame_t first, second, extra;
    assert(dart_camera_acquire_frame(camera, 20, &first) == 0);
    assert(dart_camera_acquire_frame(camera, 20, &second) == 0);
    const uint32_t previous_dumps = dump_calls;
    assert(dart_camera_acquire_frame(camera, 20, &extra) == DART_CAMERA_E_LEASE_EXHAUSTED);
    assert(dump_calls == previous_dumps && dart_camera_stop(camera) == DART_CAMERA_E_BUSY);
    assert(dart_camera_release_frame(camera, &second) == 0);
    assert(dart_camera_release_frame(camera, &first) == 0);
    wrong_pool = 1;
    assert(dart_camera_acquire_frame(camera, 20, &extra) == DART_CAMERA_E_FRAME_POOL_MISMATCH);
    assert(extra.data == NULL && dumps_held == 0 && dart_frame_lease_active(&camera->leases) == 0);
    wrong_pool = 0; bad_address = 1;
    assert(dart_camera_acquire_frame(camera, 20, &extra) == DART_CAMERA_E_FRAME_POOL_MISMATCH);
    bad_address = 0; bad_stride = 1;
    assert(dart_camera_acquire_frame(camera, 20, &extra) == DART_CAMERA_E_FRAME_GEOMETRY_MISMATCH);
    bad_stride = 0;
    assert(dart_camera_stop(camera) == 0 && dart_camera_start(camera) == 0);
    assert(map_calls == BUFFERS);
    fail_unmap = 1;
    assert(dart_camera_deinit(camera) == FAKE_FAILURE);
    assert(vicap_live && deinit_calls == 0 && live_maps == BUFFERS);
    assert(dart_media_destroy(media) == DART_CAMERA_E_BUSY);
    fail_unmap = 0; fail_deinit = 1;
    assert(dart_camera_deinit(camera) == FAKE_FAILURE);
    assert(vicap_live && live_maps == 0 && camera->state == DART_CAMERA_STATE_FAULTED);
    fail_deinit = 0;
    destroy(camera, media);

    reset_fake(); camera = initialize(&media);
    fail_map_call = 2;
    assert(dart_camera_start(camera) == DART_CAMERA_E_MAP_FAILED);
    assert(!streaming && !dumps_held && !live_maps && vicap_live);
    fail_map_call = 0;
    assert(dart_camera_start(camera) == 0);
    destroy(camera, media);

    reset_fake(); camera = initialize(&media);
    fail_map_call = 2; fail_unmap = 1;
    assert(dart_camera_start(camera) == DART_CAMERA_E_MAP_FAILED);
    assert(camera->state == DART_CAMERA_STATE_FAULTED && live_maps == 1 && !dumps_held);
    fail_unmap = 0;
    destroy(camera, media);

    reset_fake(); camera = initialize(&media);
    fail_release = 1;
    assert(dart_camera_start(camera) == FAKE_FAILURE);
    assert(streaming && dumps_held == 1 && camera->state == DART_CAMERA_STATE_FAULTED);
    fail_release = 0;
    destroy(camera, media);

    reset_fake(); camera = initialize(&media);
    wrong_pool = 1;
    assert(dart_camera_start(camera) == DART_CAMERA_E_FRAME_POOL_MISMATCH);
    assert(!streaming && !dumps_held && map_calls == 0);
    destroy(camera, media);

    reset_fake(); camera = initialize(&media);
    not_ready_count = 4;
    assert(dart_camera_start(camera) == DART_CAMERA_E_NO_FRAME);
    assert(dump_calls == 3 && !streaming && !dumps_held && map_calls == 0);
    destroy(camera, media);
    puts("camera pool regression passed: startup, leases, validation, rollback and cleanup");
    return 0;
}
