#ifndef DART_CORE_STATUS_H
#define DART_CORE_STATUS_H

#include <stdint.h>

typedef int32_t dart_status_t;
enum {
    DART_STATUS_OK = 0,
    DART_STATUS_INVALID_ARGUMENT = -1,
    DART_STATUS_INVALID_STATE = -2,
    DART_STATUS_NOT_SUPPORTED = -3,
    DART_STATUS_OUT_OF_RANGE = -4,
    DART_STATUS_TIMEOUT = -5,
    DART_STATUS_HARDWARE_ERROR = -6,
    DART_STATUS_STALE = -7,
    DART_STATUS_BUSY = -8,
    DART_STATUS_CACHE_SYNC_ERROR = -9,
    DART_STATUS_RELEASE_ERROR = -10,
    DART_STATUS_NO_MEMORY = -11,
};

enum {
    DART_OP_NONE = 0,
    DART_OP_MEDIA_INIT,
    DART_OP_MEDIA_EXIT,
    DART_OP_CAMERA_INIT,
    DART_OP_SENSOR_PROBE,
    DART_OP_SENSOR_INFO,
    DART_OP_POOL_CREATE,
    DART_OP_BLOCK_GET,
    DART_OP_MAP,
    DART_OP_UNMAP,
    DART_OP_BLOCK_RELEASE,
    DART_OP_POOL_DESTROY,
    DART_OP_DEVICE_CONFIG,
    DART_OP_CHANNEL_CONFIG,
    DART_OP_VICAP_INIT,
    DART_OP_STREAM_START,
    DART_OP_SENSOR_HANDLE,
    DART_OP_EXPOSURE_RANGE,
    DART_OP_EXPOSURE_PLAN,
    DART_OP_EXPOSURE_WRITE,
    DART_OP_FRAME_ACQUIRE,
    DART_OP_CACHE_INVALIDATE,
    DART_OP_CLOCK,
    DART_OP_FRAME_RELEASE,
    DART_OP_STREAM_STOP,
    DART_OP_VICAP_DEINIT,
};

/* Fixed, trivially copyable error payload. Text/log policy belongs to caller. */
typedef struct dart_error {
    uint32_t abi_version;
    uint32_t struct_size;
    dart_status_t status;
    uint32_t operation;
    int32_t native_code;
    int32_t cleanup_code;  /* Secondary rollback failure; 0 if none. */
} dart_error_t;

#endif
