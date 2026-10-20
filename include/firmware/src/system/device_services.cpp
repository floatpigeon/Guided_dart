#ifndef DART_SYSTEM_DEVICE_SERVICES_CPP
#define DART_SYSTEM_DEVICE_SERVICES_CPP

#include <expected>
#include <memory>

#include "camera/interface.cpp"

namespace dart::platform {

// Device selection is supplied by the process entry. Business modules use these
// callbacks without selecting a board backend or configuring hardware registers.
using CameraFactory = std::expected<std::unique_ptr<camera::ICamera>, camera::CameraError> (*)(
    const camera::Controls&) noexcept;

struct ImuOptions {
    bool enabled = false;
    int bus = -1;
    int chip_select = -2;
    const char* log_path = nullptr;
};

} // namespace dart::platform

#endif // DART_SYSTEM_DEVICE_SERVICES_CPP
