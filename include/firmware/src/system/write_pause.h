#ifndef DART_SYSTEM_WRITE_PAUSE_H
#define DART_SYSTEM_WRITE_PAUSE_H

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#define DART_DEFAULT_WRITE_PAUSE "/sdcard/app/control/write.pause"

/* Shared by the C launcher and C++ application. Any existing object pauses
 * writes. An inaccessible switch also pauses; only ENOENT permits writing. */
static inline const char* dart_write_pause_path(void) {
    const char* configured = getenv("DART_WRITE_PAUSE");
    return configured != NULL && configured[0] != '\0' ? configured : DART_DEFAULT_WRITE_PAUSE;
}

static inline int dart_writes_paused(const char* path) {
    struct stat status;
    return lstat(path, &status) == 0 || errno != ENOENT;
}

#endif
