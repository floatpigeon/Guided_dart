/* SPDX-License-Identifier: Apache-2.0 */
/* Experimental, SDK-specific SC038HGS -> MCM capture. See README.md. */
#include "mpi_sensor_api.h"
#include "mpi_sys_api.h"
#include "mpi_vb_api.h"
#include "mpi_vicap_api.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define WIDTH  640U
#define HEIGHT 480U
/* Requested DMA pitch; this does NOT assert the packing of samples. */
#define DMA_STRIDE   (WIDTH * 2U)
#define DMA_BYTES    (DMA_STRIDE * HEIGHT)
#define BLOCK_BYTES  ((DMA_BYTES * 3U + 4095U) & ~4095U)
#define MCM_SET_ATTR 0x40204315
#define DEV          VICAP_DEV_ID_0
#define CHN          VICAP_CHN_ID_0

_Static_assert(sizeof(k_vicap_mcm_chn_attr) == 32, "MCM ABI changed");
_Static_assert(offsetof(k_vicap_mcm_chn_attr, work_mode) == 28, "MCM ABI changed");
_Static_assert(VICAP_WORK_ONLY_MCM_MODE == 3, "MCM mode ABI changed");
_Static_assert(VICAP_MCM_RAW10 == 1, "MCM format ABI changed");
_Static_assert(VICAP_MCM_FRAME_COUNT == 4, "MCM ring ABI changed");
_Static_assert(VICAP_CSI2 == 3, "CSI register index changed");
_Static_assert(sizeof(k_video_frame_info) == 224, "Frame ABI changed");

static k_s32 capture_pool = -1;
static unsigned configured;
static int raw_capture;
static const char* receiver_profile = "sdk";
static k_vicap_mcm_format dma_format = VICAP_MCM_RAW10;

/* Keep experiments outside the application. The checked kernel puts cfg.stride
 * in MCM_WR0_SIZE (0x1204), whereas libisp's isp_mcm_cfg_wr uses pixel width.
 * The working offline ISP also selects format 4 and clears WR_CTRL bit 17.
 * Test those differences independently; their effect still needs hardware
 * confirmation. No address, buffer-size, or interrupt register is changed. */
static int configure_receiver_profile(void) {
    if (!strcmp(receiver_profile, "sdk"))
        return 0;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        return -1;
    volatile k_u32* regs =
        mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0x90000000ULL);
    const int map_errno = errno;
    close(fd);
    if (!regs || regs == MAP_FAILED) {
        errno = map_errno;
        return -1;
    }
    int ret = -1;
    const k_u32 ctrl = regs[0x1200 / 4];
    const k_u32 size = regs[0x1204 / 4];
    const k_u32 mi = regs[0x1604 / 4];
    if (configured != 1 || size != ((HEIGHT << 16) | DMA_STRIDE)
        || ctrl != (0x20000U | ((k_u32)dma_format << 5)) || mi != ((k_u32)dma_format << 4)
        || regs[0x1618 / 4] != DMA_BYTES || regs[0x1620 / 4] != DMA_STRIDE) {
        fprintf(
            stderr,
            "receiver profile=%s unexpected SDK configuration ctrl=%08x size=%08x mi=%08x\n",
            receiver_profile, ctrl, size, mi);
        goto done;
    }
    const k_u32 corrected_size = (HEIGHT << 16) | WIDTH;
    k_u32 corrected_ctrl = ctrl;
    if (!strcmp(receiver_profile, "width16-input"))
        corrected_ctrl = (ctrl & ~0xe0U) | (VICAP_MCM_RAW16 << 5);
    if (!strcmp(receiver_profile, "width16-ctrl"))
        corrected_ctrl &= ~0x20000U;
    regs[0x1204 / 4] = corrected_size;
    regs[0x1200 / 4] = corrected_ctrl;
    __sync_synchronize();
    if (regs[0x1204 / 4] != corrected_size || regs[0x1200 / 4] != corrected_ctrl)
        goto done;
    fprintf(
        stderr,
        "receiver profile=%s applied size=%08x->%08x ctrl=%08x->%08x dma_format=%u pitch=%u\n",
        receiver_profile, size, corrected_size, ctrl, corrected_ctrl, (unsigned)dma_format,
        DMA_STRIDE);
    ret = 0;
done:
    if (munmap((void*)regs, 0x2000))
        ret = -1;
    return ret;
}

static void notify_complete(void) {
    const char* path = getenv("DART_EXIT_MARKER");
    if (!path || !*path)
        return;
    FILE* marker = fopen(path, "w");
    if (!marker)
        return;
    fprintf(marker, "%ld\n", (long)getpid());
    fflush(marker);
    fsync(fileno(marker));
    fclose(marker);
}

/* Read-only snapshots of the register blocks used by the checked kernel's
 * kd_vi_set_config / vicap_set_mcm_conf / isp_hw_mcm_irq_isr. CSI enums are
 * one-based; CSI2's DWC block is VI + (VICAP_CSI2 << 11). */
static void register_snapshot(
    const char* stage, const char* block, k_u64 base, size_t bytes, const k_u32* offsets,
    size_t count) {
    /* MPI mmap only accepts VB memory on this firmware. The RT-Smart mem
     * device implements RT_FIOMMAP2 for peripheral addresses; O_SYNC selects
     * uncached mappings. Never write through this diagnostic mapping. */
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        fprintf(
            stderr, "receiver stage=%s block=%s open_mem_failed errno=%d\n", stage, block, errno);
        return;
    }
    volatile k_u32* mapped = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, (off_t)base);
    const int map_errno = errno;
    close(fd);
    if (!mapped || mapped == MAP_FAILED) {
        fprintf(
            stderr, "receiver stage=%s block=%s map_failed errno=%d\n", stage, block, map_errno);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        fprintf(
            stderr, "receiver stage=%s block=%s reg=0x%04x value=0x%08x\n", stage, block,
            offsets[i], mapped[offsets[i] / sizeof(k_u32)]);
    }
    int ret = munmap((void*)mapped, bytes);
    if (ret)
        fprintf(stderr, "receiver block=%s unmap_failed result=%d\n", block, ret);
}

static void receiver_diagnostics(const char* stage) {
    static const k_u32 vi[] = {
        0x0010, 0x0060, 0x1800, 0x1804, 0x1808, 0x180c, 0x1840, 0x1844, 0x1848, 0x184c, 0x1880,
        0x1884, 0x1888, 0x1890, 0x18e0, 0x18f0, 0x1900, 0x1910, 0x1920, 0x1930, 0x1940,
    };
    static const k_u32 mcm[] = {
        0x0400, 0x0404, 0x1200, 0x1204, 0x1300, 0x1600, 0x1604, 0x1608, 0x160c, 0x1610,
        0x1618, 0x1620, 0x1624, 0x1628, 0x1694, 0x16c0, 0x16c4, 0x16c8, 0x16d0, 0x16e8,
    };
    register_snapshot(stage, "vi_csi2", 0x90009000ULL, 0x2000, vi, sizeof(vi) / sizeof(vi[0]));
    register_snapshot(stage, "mcm", 0x90000000ULL, 0x2000, mcm, sizeof(mcm) / sizeof(mcm[0]));
}

static void sensor_diagnostics(int fd) {
    static const k_u32 addresses[] = {
        0x2100, 0x3031, 0x3037, 0x4b01, 0x4603, 0x3222, 0x3225, 0x3223, 0x3282,
        0x4501, 0x3208, 0x3209, 0x320a, 0x320b, 0x320c, 0x320d, 0x320e, 0x320f,
    };
    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        k_u32 value = 0;
        int ret = kd_mpi_sensor_reg_read(fd, addresses[i], &value);
        fprintf(
            stderr, "sensor reg=0x%04x value=0x%02x result=%d errno=%d\n", addresses[i], value, ret,
            ret ? errno : 0);
    }
}

int __real_ioctl(int fd, int request, ...);

/* This executable alone overrides the vendor MPI's default MCM format/pitch.
 * It neither patches libvicap.a nor invents a public RAW dump API. */
int __wrap_ioctl(int fd, int request, ...) {
    va_list args;
    va_start(args, request);
    void* arg = va_arg(args, void*);
    va_end(args);
    if (request != MCM_SET_ATTR || !raw_capture)
        return __real_ioctl(fd, request, arg);

    k_vicap_mcm_chn_attr attr;
    if (!arg || capture_pool < 0) {
        errno = EINVAL;
        return -1;
    }
    memcpy(&attr, arg, sizeof(attr));
    if (configured || attr.work_mode != VICAP_WORK_ONLY_MCM_MODE || attr.mcm_chn != CHN
        || attr.cfg.width != WIDTH || attr.cfg.height != HEIGHT || attr.cfg.format != 0
        || attr.cfg.stride != WIDTH * 3U) {
        fprintf(stderr, "Unexpected MCM request: SDK adapter must be reviewed\n");
        errno = EINVAL;
        return -1;
    }
    attr.cfg.format = dma_format;
    attr.cfg.stride = DMA_STRIDE;
    attr.buffer_pool_id = capture_pool;
    attr.buff_num = VICAP_MCM_FRAME_COUNT;
    fprintf(
        stderr, "Request MCM format=%u: %ux%u, DMA pitch %u, pool %d\n", (unsigned)dma_format,
        WIDTH, HEIGHT, DMA_STRIDE, capture_pool);
    int ret = __real_ioctl(fd, request, &attr);
    if (!ret)
        ++configured;
    return ret;
}

static int save_frame(FILE* out, FILE* metadata, const k_video_frame_info* info, int gradient) {
    const k_video_frame* f = &info->v_frame;
    const unsigned data_stride = dma_format == VICAP_MCM_RAW10 ? WIDTH / 4U * 5U : DMA_STRIDE;
    const unsigned data_bytes = data_stride * HEIGHT;
    /* Vendor MCM metadata still labels buffers YUV444 and does not reliably
     * initialize stride[]. Keep it unchanged for release; use our DMA pitch. */
    if (configured != 1 || f->width != WIDTH || f->height != HEIGHT || !f->phys_addr[0]) {
        fprintf(stderr, "Unexpected MCM frame geometry/address\n");
        return -1;
    }
    k_vb_blk_handle block = kd_mpi_vb_phyaddr_to_handle(f->phys_addr[0]);
    if (block == VB_INVALID_HANDLE || kd_mpi_vb_handle_to_pool_id(block) != capture_pool
        || kd_mpi_vb_handle_to_phyaddr(block) != f->phys_addr[0]) {
        fprintf(stderr, "Frame does not point to our allocated VB block\n");
        return -1;
    }
    void* data = kd_mpi_sys_mmap(f->phys_addr[0], DMA_BYTES);
    if (!data || data == (void*)-1)
        return -1;
    size_t written = fwrite(data, 1, data_bytes, out);
    int ret = written == data_bytes ? 0 : -1;
    if (kd_mpi_sys_munmap(data, DMA_BYTES))
        ret = -1;
    if (fprintf(
            metadata,
            "{\n  \"sensor\": \"SC038HGS mono CSI2\",\n"
            "  \"width\": %u, \"height\": %u,\n"
            "  \"requested_mcm_format\": %u, \"requested_stride\": %u,\n"
            "  \"data_stride\": %u,\n"
            "  \"receiver_profile\": \"%s\",\n"
            "  \"bytes\": %u, \"gradient\": %s,\n"
            "  \"packing\": \"%s\",\n"
            "  \"sdk_pixel_format\": %u, \"pts\": %llu\n}\n",
            WIDTH, HEIGHT, (unsigned)dma_format, DMA_STRIDE, data_stride, receiver_profile,
            data_bytes, gradient ? "true" : "false",
            dma_format == VICAP_MCM_RAW10 ? "packed10-lsb" : "unverified",
            (unsigned)f->pixel_format, (unsigned long long)f->pts)
        < 0)
        ret = -1;
    return ret;
}

/* Reproduce the pre-RAW application's offline ISP -> NV12 path. Only save
 * its luma plane as a PGM so it can be inspected without interpreting RAW. */
static int save_isp_frame(FILE* out, FILE* metadata, const k_video_frame_info* info) {
    const k_video_frame* f = &info->v_frame;
    const k_u32 stride = f->stride[0];
    if (f->width != WIDTH || f->height != HEIGHT
        || f->pixel_format != PIXEL_FORMAT_YUV_SEMIPLANAR_420 || !f->phys_addr[0] || stride < WIDTH
        || stride > WIDTH * 4U) {
        fprintf(
            stderr, "Unexpected ISP output geometry/format/stride: %ux%u format=%u stride=%u\n",
            f->width, f->height, f->pixel_format, stride);
        return -1;
    }
    size_t bytes = (size_t)stride * HEIGHT;
    const unsigned char* data = kd_mpi_sys_mmap(f->phys_addr[0], bytes);
    if (!data || data == (void*)-1)
        return -1;
    int ret = fprintf(out, "P5\n%u %u\n255\n", WIDTH, HEIGHT) < 0 ? -1 : 0;
    for (k_u32 row = 0; !ret && row < HEIGHT; ++row) {
        if (fwrite(data + (size_t)row * stride, 1, WIDTH, out) != WIDTH)
            ret = -1;
    }
    if (kd_mpi_sys_munmap((void*)data, bytes))
        ret = -1;
    if (fprintf(
            metadata,
            "{\"path\":\"offline-isp-luma\",\"width\":%u,\"height\":%u,"
            "\"stride\":%u,\"pts\":%llu}\n",
            WIDTH, HEIGHT, stride, (unsigned long long)f->pts)
        < 0)
        ret = -1;
    return ret;
}

static double now_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    atexit(notify_complete);
    if ((argc != 3 && argc != 4)
        || (strcmp(argv[1], "gradient") && strcmp(argv[1], "image") && strcmp(argv[1], "isp"))) {
        fprintf(
            stderr,
            "Usage: %s gradient|image|isp output-file "
            "[sdk|width10|width16|width16-input|width16-ctrl]\n",
            argv[0]);
        return 1;
    }
    raw_capture = strcmp(argv[1], "isp") != 0;
    if (argc == 4)
        receiver_profile = argv[3];
    if ((strcmp(receiver_profile, "sdk") && strcmp(receiver_profile, "width10")
         && strcmp(receiver_profile, "width16") && strcmp(receiver_profile, "width16-input")
         && strcmp(receiver_profile, "width16-ctrl"))
        || (!raw_capture && strcmp(receiver_profile, "sdk"))) {
        fprintf(stderr, "Unsupported receiver profile\n");
        return 1;
    }
    if (!strcmp(receiver_profile, "width16") || !strcmp(receiver_profile, "width16-ctrl"))
        dma_format = VICAP_MCM_RAW16;
    const int gradient = !strcmp(argv[1], "gradient");
    fprintf(
        stderr, "probe mode=%s path=%s profile=%s\n", argv[1],
        raw_capture ? "only-mcm-raw" : "offline-isp-nv12", receiver_profile);
    char metadata_path[512];
    int length = snprintf(metadata_path, sizeof(metadata_path), "%s.json", argv[2]);
    if (length < 0 || (size_t)length >= sizeof(metadata_path))
        return 1;
    /* Exclusive creation preserves earlier captures. */
    FILE* out = fopen(argv[2], "wbx");
    if (!out) {
        perror(argv[2]);
        return 1;
    }
    FILE* metadata = fopen(metadata_path, "wx");
    if (!metadata) {
        perror(metadata_path);
        fclose(out);
        remove(argv[2]);
        return 1;
    }

    k_bool vb_ready = K_FALSE, initialized = K_FALSE;
    k_bool started = K_FALSE, sensor_ready = K_FALSE;
    k_vicap_sensor_attr sensor = {.dev_num = DEV};
    k_vicap_sensor_info sensor_info = {0};
    k_vicap_dev_attr dev = {0};
    k_vicap_chn_attr channel = {0};
    k_vb_config vb = {.max_pool_cnt = 16};
    k_vb_pool_config pool = {
        .blk_size = BLOCK_BYTES, .blk_cnt = VICAP_MCM_FRAME_COUNT, .mode = VB_REMAP_MODE_NOCACHE};
    const k_vicap_sensor_type type = SC038HGS_MIPI_CSI2_640X480_10BIT_LINEAR;
    k_s32 ret = -1;
#define CHECK(call)                                                              \
    do {                                                                         \
        errno = 0;                                                               \
        ret = (call);                                                            \
        fprintf(stderr, "%s result=%d errno=%d\n", #call, ret, ret ? errno : 0); \
        if (ret)                                                                 \
            goto done;                                                           \
    } while (0)
    CHECK(kd_mpi_vicap_get_sensor_info(type, &sensor_info));
    fprintf(
        stderr, "sensor name=%s database=%s csi=%u source=%u lanes=%u phy=%u type=0x%x hdr=%u\n",
        sensor_info.sensor_name, sensor_info.database_name, sensor_info.csi_num,
        sensor_info.source_id, sensor_info.mipi_lanes, sensor_info.phy_freq, sensor_info.data_type,
        sensor_info.hdr_mode);
    if (sensor_info.width != WIDTH || sensor_info.height != HEIGHT
        || sensor_info.data_type != VICAP_CSI_DATA_TYPE_RAW10) {
        ret = -1;
        goto done;
    }
    CHECK(kd_mpi_vb_set_config(&vb));
    CHECK(kd_mpi_vb_init());
    vb_ready = K_TRUE;
    if (raw_capture) {
        capture_pool = kd_mpi_vb_create_pool(&pool);
        if (capture_pool < 0) {
            ret = capture_pool;
            goto done;
        }
    }
    dev.acq_win.width = WIDTH;
    dev.acq_win.height = HEIGHT;
    dev.input_type = VICAP_INPUT_TYPE_SENSOR;
    dev.mode = raw_capture ? VICAP_WORK_ONLY_MCM_MODE : VICAP_WORK_OFFLINE_MODE;
    dev.sensor_info = sensor_info;
    dev.buffer_num = VICAP_MCM_FRAME_COUNT;
    dev.buffer_size = raw_capture ? BLOCK_BYTES : ((DMA_BYTES + 4095U) & ~4095U);
    dev.buffer_pool_id = capture_pool;
    if (!raw_capture) {
        dev.pipe_ctrl.data = 0xffffffffU;
        dev.pipe_ctrl.bits.ae_enable = K_FALSE;
        dev.pipe_ctrl.bits.awb_enable = K_TRUE;
        dev.pipe_ctrl.bits.ahdr_enable = K_FALSE;
        dev.pipe_ctrl.bits.dnr3_enable = K_TRUE;
        /* Use the same deployed database as the earlier ISP application,
         * rather than the SDK's unrelated /bin/ default scene. */
        CHECK(kd_mpi_vicap_register_scene("dart_probe", "/sdcard/app/isp/"));
        CHECK(kd_mpi_vicap_load_scene("dart_probe"));
        CHECK(kd_mpi_vicap_set_database_parse_mode(DEV, VICAP_DATABASE_PARSE_XML_JSON));
    }
    CHECK(kd_mpi_vicap_set_dev_attr(DEV, dev));
    channel.out_win = dev.acq_win;
    channel.crop_win = channel.scale_win = channel.out_win;
    channel.chn_enable = K_TRUE;
    channel.pix_format =
        raw_capture ? PIXEL_FORMAT_RGB_BAYER_10BPP : PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    channel.scale_enable = !raw_capture;
    channel.buffer_num = VICAP_MCM_FRAME_COUNT;
    channel.buffer_size = raw_capture ? BLOCK_BYTES : ((WIDTH * HEIGHT * 3U / 2U + 4095U) & ~4095U);
    channel.buffer_pool_id = capture_pool;
    channel.alignment = 12;
    CHECK(kd_mpi_vicap_set_chn_attr(DEV, CHN, channel));
    initialized = K_TRUE;
    CHECK(kd_mpi_vicap_init(DEV));
    if (raw_capture && configured != 1) {
        fprintf(stderr, "RAW10 MCM configuration was not applied\n");
        ret = -1;
        goto done;
    }
    CHECK(kd_mpi_vicap_get_sensor_fd(&sensor));
    sensor_ready = K_TRUE;
    receiver_diagnostics("initialized");
    if (raw_capture)
        CHECK(configure_receiver_profile());
    /* The checked MPI start call starts MCM, powers/initializes the sensor,
     * then enables its stream. Applying the pattern before it is lost when
     * that sensor init resets the registers. */
    started = K_TRUE; /* A partial MPI start also needs receiver cleanup. */
    CHECK(kd_mpi_vicap_start_stream(DEV));
    k_sensor_test_pattern pattern = {.enable = gradient};
    CHECK(kd_mpi_sensor_tpg_set(sensor.sensor_fd, pattern));
    sensor_diagnostics(sensor.sensor_fd);
    receiver_diagnostics("started");
    double first_time = 0, last_time = 0;
    for (unsigned i = 0; i < 8; ++i) {
        k_video_frame_info frame = {0};
        /* Required by this MPI's MCM dispatch. This does not request a color
         * conversion: the kernel receives the MCM dump ioctl directly. */
        errno = 0;
        ret = kd_mpi_vicap_dump_frame(
            DEV, CHN, raw_capture ? VICAP_DUMP_YUV444 : VICAP_DUMP_YUV, &frame, 3000);
        last_time = now_seconds();
        if (!i)
            first_time = last_time;
        fprintf(
            stderr,
            "dump index=%u result=%d errno=%d width=%u height=%u format=%u stride=%u pts=%llu\n", i,
            ret, ret ? errno : 0, frame.v_frame.width, frame.v_frame.height,
            frame.v_frame.pixel_format, frame.v_frame.stride[0],
            (unsigned long long)frame.v_frame.pts);
        if (ret) {
            receiver_diagnostics("timeout");
            goto done;
        }
        int saved = i == 7 ? (raw_capture ? save_frame(out, metadata, &frame, gradient)
                                          : save_isp_frame(out, metadata, &frame))
                           : 0;
        ret = kd_mpi_vicap_dump_release(DEV, CHN, &frame);
        if (saved || ret) {
            fprintf(stderr, "Save/release failed: %d/%d\n", saved, ret);
            ret = -1;
            goto done;
        }
    }
    receiver_diagnostics("received");
    if (last_time > first_time)
        fprintf(stderr, "capture arrival_fps=%.3f frames=8\n", 7.0 / (last_time - first_time));
    ret = 0;
done:
#define CLEAN(call)                                                                               \
    do {                                                                                          \
        fprintf(stderr, "cleanup call=%s begin\n", #call);                                        \
        errno = 0;                                                                                \
        int cleanup = (call);                                                                     \
        fprintf(                                                                                  \
            stderr, "cleanup call=%s result=%d errno=%d\n", #call, cleanup, cleanup ? errno : 0); \
        if (cleanup)                                                                              \
            ret = -1;                                                                             \
    } while (0)
    if (sensor_ready)
        CLEAN(kd_mpi_sensor_stream_enable(sensor.sensor_fd, K_FALSE));
    if (started)
        CLEAN(kd_mpi_vicap_stop_stream(DEV));
    if (sensor_ready) {
        k_sensor_test_pattern normal = {0};
        CLEAN(kd_mpi_sensor_tpg_set(sensor.sensor_fd, normal));
        CLEAN(kd_mpi_sensor_power_set(sensor.sensor_fd, K_FALSE));
    }
    if (initialized)
        CLEAN(kd_mpi_vicap_deinit(DEV));
    if (capture_pool >= 0)
        CLEAN(kd_mpi_vb_destory_pool((k_u32)capture_pool));
    if (vb_ready)
        CLEAN(kd_mpi_vb_exit());
    if (fclose(out))
        ret = -1;
    if (fclose(metadata))
        ret = -1;
    if (ret) {
        remove(argv[2]);
        remove(metadata_path);
    } else {
        if (raw_capture)
            printf(
                "Saved %u unprocessed MCM bytes to %s\n",
                (dma_format == VICAP_MCM_RAW10 ? WIDTH / 4U * 5U : DMA_STRIDE) * HEIGHT, argv[2]);
        else
            printf("Saved offline ISP luma PGM to %s\n", argv[2]);
    }
    fprintf(stderr, "probe_complete result=%d\n", ret);
    (void)fflush(NULL);
    (void)fsync(STDOUT_FILENO);
    (void)fsync(STDERR_FILENO);
    return ret ? 1 : 0;
}
