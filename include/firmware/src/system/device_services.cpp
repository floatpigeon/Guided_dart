#ifndef DART_SYSTEM_DEVICE_SERVICES_CPP
#define DART_SYSTEM_DEVICE_SERVICES_CPP

#include <expected>
#include <memory>

#include "camera/interface.cpp"
#include "imu/sample.cpp"
#include "pwm/interface.cpp"
#include "system/write_control.cpp"

namespace dart::platform {

// Device selection is supplied by the process entry. Business modules use these
// callbacks without selecting a board backend or configuring hardware registers.
using CameraFactory = std::expected<std::unique_ptr<camera::ICamera>, camera::CameraError> (*)(
    const camera::Controls&) noexcept;
using PwmFactory = std::expected<std::unique_ptr<pwm::IPwm>, pwm::Error> (*)() noexcept;

struct ImuOptions {
    bool enabled = false;
    int bus = -1;
    int chip_select = -2;
    const char* log_path = nullptr;
    bool ignore_device_id = false;
    WriteControl* writes = nullptr;
    void (*sample_observer)(const imu::Sample&, void*) noexcept = nullptr;
    void* sample_observer_context = nullptr;
};

} // namespace dart::platform

#endif // DART_SYSTEM_DEVICE_SERVICES_CPP
