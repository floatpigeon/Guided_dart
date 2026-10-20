#ifndef DART_INCLUDE_FIRMWARE_APP_SRC_IMU_INTERFACE_CPP
#define DART_INCLUDE_FIRMWARE_APP_SRC_IMU_INTERFACE_CPP

#include <cstdint>
#include <expected>

#include "sample.cpp"

namespace dart::imu {

enum class ErrorCode : std::uint8_t {
    kInvalidConfiguration,
    kAlreadyInitialized,
    kNotInitialized,
    kNoMemory,
    kBusTransfer,
    kUnexpectedDeviceId,
    kClock,
};

class IImu {
public:
    virtual ~IImu() = default;
    virtual std::expected<Sample, ErrorCode> read_sample() noexcept = 0;
};

} // namespace dart::imu

#endif // DART_INCLUDE_FIRMWARE_APP_SRC_IMU_INTERFACE_CPP
