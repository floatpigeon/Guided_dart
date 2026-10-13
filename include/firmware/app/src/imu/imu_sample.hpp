#ifndef DART_IMU_SAMPLE_HPP
#define DART_IMU_SAMPLE_HPP

#include <cstdint>
#include <expected>

/*
 * Device-side IMU value types and the sample-source interface.
 *
 * This header is the stable abstraction the rest of the system depends on: it
 * carries no POSIX, K230 SDK or C driver header, so consumers (estimator,
 * control, logging) never inherit the hardware ABI. The K230 C backend lives in
 * spi/iim42622.h and is pulled in only by the adapter header
 * imu/k230/iim42622.hpp.
 */

namespace dart::imu {

struct Vector3f {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Quaternionf {
    /* Rotation from sensor/body axes into the selected reference frame. */
    float w = 1.0F;
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Vector3i16 {
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::int16_t z = 0;
};

struct RawCounts {
    Vector3i16 accelerometer{};
    Vector3i16 gyroscope{};
};

enum class SampleFlag : std::uint32_t {
    kNone = 0U,
    kAccelerometerSaturated = 1U << 0U,
    kGyroscopeSaturated = 1U << 1U,
};

constexpr std::uint32_t operator|(SampleFlag lhs, SampleFlag rhs) noexcept {
    return static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs);
}

constexpr bool has_flag(std::uint32_t flags, SampleFlag flag) noexcept {
    return (flags & static_cast<std::uint32_t>(flag)) != 0U;
}

struct Sample {
    RawCounts raw_counts{};
    /* Specific force in m/s^2 and angular rate in rad/s, both sensor/body axes. */
    Vector3f acceleration_m_s2{};
    Vector3f angular_velocity_rad_s{};
    float temperature_c = 0.0F;
    std::uint64_t timestamp_ns = 0U; /* CLOCK_MONOTONIC domain. */
    std::uint32_t flags = 0U;
};

enum class ErrorCode : std::uint8_t {
    kInvalidConfiguration,
    kAlreadyInitialized,
    kNotInitialized,
    kNoMemory,
    kBusTransfer,
    kUnexpectedDeviceId,
    kClock,
};

/* Hardware-facing sample source. Lifecycle and all estimation stay out of this interface. */
class IImu {
public:
    virtual ~IImu() = default;
    virtual std::expected<Sample, ErrorCode> read_sample() noexcept = 0;
};

}  // namespace dart::imu

#endif
