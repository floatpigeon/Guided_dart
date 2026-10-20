/* SDK-specific RAW10 MCM adapter. The checked library hashes live in
 * include/CMakeLists.txt; keep tools/sc038hgs_raw as the board packing probe. */
#include "camera/k230/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mpi_vicap_api.h"

#define DART_MCM_SET_ATTR 0x40204315

_Static_assert(sizeof(k_vicap_mcm_chn_attr) == 32, "MCM ABI changed");
_Static_assert(offsetof(k_vicap_mcm_chn_attr, work_mode) == 28, "MCM ABI changed");
_Static_assert(VICAP_WORK_ONLY_MCM_MODE == 3, "MCM mode ABI changed");
_Static_assert(VICAP_MCM_RAW10 == 1, "MCM format ABI changed");
_Static_assert(VICAP_MCM_FRAME_COUNT == 4, "MCM ring ABI changed");
_Static_assert(sizeof(k_video_frame_info) == 224, "Frame ABI changed");

/* All camera control calls are serialized by the media owner. Never intercept
 * unrelated ioctls or retain this context outside the two MPI init calls. */
static dart_camera_t* configuring;
static unsigned configured;

int __real_ioctl(int fd, int request, ...);

int __wrap_ioctl(int fd, int request, ...) {
    va_list args;
    va_start(args, request);
    void* arg = va_arg(args, void*);
    va_end(args);
    if (request != DART_MCM_SET_ATTR || configuring == NULL)
        return __real_ioctl(fd, request, arg);

    k_vicap_mcm_chn_attr attr;
    if (arg == NULL) {
        errno = EINVAL;
        return -1;
    }
    memcpy(&attr, arg, sizeof(attr));
    const dart_camera_t* camera = configuring;
    if (configured != 0u || camera->raw_pool_id < 0 || attr.work_mode != VICAP_WORK_ONLY_MCM_MODE
        || attr.mcm_chn != VICAP_CHN_ID_0 || attr.cfg.width != camera->config.sensor_width
        || attr.cfg.height != camera->config.sensor_height || attr.cfg.format != 0u
        || attr.cfg.stride != attr.cfg.width * 3u) {
        errno = EINVAL;
        return -1;
    }
    attr.cfg.format = VICAP_MCM_RAW10;
    /* Retain the board-tested conservative DMA span. MCM RAW10 actually
     * writes compact width*5/4 rows; camera->raw_stride describes that data.
     * The checked kernel incorrectly also uses this span as the input width;
     * correct only that register after init, before starting the receiver. */
    attr.cfg.stride = attr.cfg.width * 2u;
    attr.buff_num = dart_camera_buffer_count(camera);
    attr.buffer_pool_id = camera->raw_pool_id;
    const int rc = __real_ioctl(fd, request, &attr);
    if (rc == 0)
        ++configured;
    return rc;
}

static int correct_mcm_input_width(const dart_camera_t* camera) {
    const uint32_t width = camera->config.sensor_width;
    const uint32_t height = camera->config.sensor_height;
    const uint32_t span = width * 2u;
    const uint32_t expected = (height << 16u) | span;
    const uint32_t corrected = (height << 16u) | width;
    const int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        return DART_CAMERA_E_MAP_FAILED;
    volatile uint32_t* regs =
        mmap(NULL, 0x2000u, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0x90000000ULL);
    close(fd);
    if (regs == NULL || regs == MAP_FAILED)
        return DART_CAMERA_E_MAP_FAILED;
    int rc = DART_CAMERA_E_NOT_SUPPORTED;
    /* Guard the private register workaround against a different firmware or
     * another owner. Do not change addresses, formats, IRQs or DMA limits. */
    if (regs[0x1204u / 4u] == expected && regs[0x1200u / 4u] == (0x20000u | (VICAP_MCM_RAW10 << 5u))
        && regs[0x1604u / 4u] == (VICAP_MCM_RAW10 << 4u) && regs[0x1618u / 4u] == span * height
        && regs[0x1620u / 4u] == span) {
        regs[0x1204u / 4u] = corrected;
        __sync_synchronize();
        if (regs[0x1204u / 4u] == corrected)
            rc = DART_CAMERA_OK;
    }
    if (munmap((void*)regs, 0x2000u) != 0)
        return DART_CAMERA_E_MAP_FAILED;
    return rc;
}

int dart_camera_initialize_mcm(dart_camera_t* camera, k_vicap_chn_attr channel) {
    if (configuring != NULL)
        return DART_CAMERA_E_BUSY;
    configuring = camera;
    configured = 0;
    int rc = kd_mpi_vicap_set_chn_attr(VICAP_DEV_ID_0, VICAP_CHN_ID_0, channel);
    if (rc == 0) {
        /* Even a failed SDK init may require deinit. */
        camera->vicap_initialized = 1;
        rc = kd_mpi_vicap_init(VICAP_DEV_ID_0);
    }
    if (rc == 0 && configured != 1u)
        rc = DART_CAMERA_E_NOT_SUPPORTED;
    if (rc == 0)
        rc = correct_mcm_input_width(camera);
    configuring = NULL;
    configured = 0;
    return rc;
}
