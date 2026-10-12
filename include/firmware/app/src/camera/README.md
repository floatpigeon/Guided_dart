# Camera ACL

> Layout follows librmcs: `camera.hpp` / `error.*` / `error_codes.h` are the
> platform-free interface and error layers, `c_api/` is the C ABI plus its
> platform-free implementations, and `k230/` is the only place that may include
> SDK headers or the C ABI. The current C boundary is
> [c_api/camera.h](c_api/camera.h) and [error_codes.h](error_codes.h); the C++
> entry point is `dart::camera::k230::create<N>()` in
> [k230/camera.hpp](k230/camera.hpp). Historical validation text below is not
> evidence for the current code. No board execution is validated:
> **BLOCKED: hardware validation pending**.

The C data plane owns camera initialization, frame leases, mappings and
DMA cache synchronization. The C++ control plane exposes `ICamera`
(`camera.hpp`), implemented for this board by `dart::camera::k230::Camera<N>`.
Only the C data-plane implementation includes K230 or POSIX device/runtime headers. There is no
host implementation of MPP: host builds compile pure configuration, exposure
calculation and lease logic only.

## Exposure

The v1 upper layers received an injected `IExposureControl &` and called one
virtual function (historical example; the replacement interface lands in T7):

```cpp
auto result = exposure.set_exposure_us(requested_us);
if (!result) {
    // The caller decides whether to retry, disable capture or report the error.
}
```

The v1 composition root constructed `CameraExposureControl` from a camera handle.
Its headers have been removed; the replacement interface and adapter land in T7.
The camera must outlive a non-owning adapter. Calls to camera/media objects and
the adapter must be serialized.

Fixed mode disables AE at device initialization. After stream start, the ACL
gets the sensor fd, queries the range in microseconds, clamps the command and
converts to seconds for `kd_mpi_sensor_intg_time_set`. Fractional limits round
inward: 27.375..999.75 us permits integer commands 28..999 us. Invalid or failed
range queries are errors, not silent fallback. `applied_us` is the accepted
command, not a measured sensor readback; line quantization remains sensor-owned.
Changing AUTO/FIXED mode requires stop/deinit/init. Zero is not an AE switch.

In fixed mode, `initial_exposure_us=0` allows start but blocks frame acquisition
until a successful exposure call. A nonzero initial command is applied during
start; failure rolls back the stream and records any rollback error.

## Ownership

Create one `dart_media_t` at startup and inject it into the camera. It owns global
VB initialization/exit. Do not coexist with independently initialized raw SDK
VB users, create multiple media owners, or destroy it before all consumers have
finished. This slice reserves DEV0/CHN0 and allows one attached camera per owner.
No application-level mutable global state is used to pretend these resources
are independent.

The camera owns a private output pool and maps every block before streaming.
Acquisition returns read-only Y8 luma, not full NV12. DMA writes the pixels; the
ACL invalidates cache on every acquisition. No application allocation, mapping,
unmapping or pixel copy occurs in acquire/release. `stride` may exceed `width`.
The caller injects a lease capacity: up to `max_outstanding_frames` frames may be
outstanding at once, each carrying a distinct token, and release order is free. A
lease carries owner identity and token; stale/double/wrong-owner releases are
rejected. SDK release failure retains the lease so the same frame can be retried.
Stop/deinit return BUSY while any lease is outstanding. Copies of frame structs
are aliases, not additional references.

Capacity is bounded twice: by the ceiling in `error_codes.h`
(`DART_CAMERA_MAX_OUTSTANDING_FRAMES`) and by `channel_buffers`, because each held
lease keeps one driver buffer busy. Requesting more is a configuration error.
When every slot is already held, acquire does not touch the driver at all: it
reports `DART_CAMERA_E_LEASE_EXHAUSTED`, the frame stays in the driver pool and is
recycled there. That is a drop, not a failure, so a pipeline counts it instead of
logging an error. On the C++ side the capacity is a compile-time parameter of the
factory (`dart::camera::k230::create<N>`), which sizes the lease slots statically and
removes the per-frame allocation; `capacity()` and `outstanding()` let a producer
apply its own bounded-drop policy before calling `acquire()`.

A lease must be released before the camera is destroyed. Destroying the camera
first does not free the frame — the data plane refuses to stop while a lease is
active — so such a lease is detached and a later release reports a stale lease
instead of reaching into a destroyed object. The SDK handles are retained for an
explicit retry rather than dropped silently.

Startup/shutdown may allocate/free. Shutdown order is release frame, camera
deinit, camera destroy, media destroy. Failed cleanup retains handles and pool
ownership; retry deinit, do not free or close the media owner. Internal rollback
failure sets FAULTED, preserves the primary and cleanup native codes and permits
cleanup retries. Objects must not be used after successful destroy.

Sensor mode and CSI are exact-match validated; SDK fallback is rejected. CSI
port numbering is 0/1/2. Output width must be a multiple of 32, height must be
even, and output cannot upscale. No default sensor mode or calibrated exposure
is invented. HDR is unsupported in this slice.

`received_us` is CLOCK_MONOTONIC receipt time, not exposure time. `source_pts`
is the untouched SDK timestamp with SOURCE_PTS_UNVALIDATED set. Its timebase,
sampling epoch and relationship to IMU time must be established on the board
before it enters control or tracking.

## Build

Host:

```sh
cmake -S . -B /tmp/dart-camera-host -DDART_CAMERA_K230=OFF
cmake --build /tmp/dart-camera-host --parallel
```

Board targets use installed SDK headers/libraries and an explicit toolchain
once T2..T7 provide their implementations:

```sh
cmake -S . -B /tmp/dart-camera-cross \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv64-musl.cmake \
  -DK230_TOOLCHAIN_DIR=/path/to/riscv64-musl/bin \
  -DK230_SDK_ROOT=/mnt/mydata/kRTOSSDK \
  -DDART_CAMERA_K230=ON
cmake --build /tmp/dart-camera-cross \
  --target dart_camera_k230 dart_camera_acl
```

`K230_MPP_LIB_DIR` and `K230_LINK_SCRIPT` are explicit override points for other
SDK builds. The C++ ACL uses C++23 and `-fno-exceptions -fno-rtti`.
No C++23 fallback implementation is introduced. The v1 probe targets are removed.

## Validation

Host tests check C11 layout, C++23 expected, configuration bounds, fractional
clamping, microseconds-to-seconds conversion, invalid/nonfinite ranges, frame
lease transitions and adapter error propagation. Explicit test fakes are not
MPP compatibility code and cannot validate DMA or SDK rollback behavior.

On 2026-10-10, all four GNU 13.3 host tests passed. The installed RISC-V musl
GCC 12.0.1 toolchain compiled and statically linked the C camera probe, real C++
adapter probe and expected probe with the local SDK. Actual board execution is
**BLOCKED: hardware validation pending**.

The v1 diagnostic executable is removed. A v2 board diagnostic is outside this
round; choose a sensor-supported mode and calibrated exposure for board validation.

Board acceptance must verify private pool compatibility, cache freshness under
DMA, stable frame timing, actual exposure/line quantization, warm restart and
cleanup/release failures. Correlate raw PTS with a known timing reference and
inspect real images; compile/link success and a single luma sample are not
evidence of visual correctness or performance.
