#ifndef DART_INCLUDE_FIRMWARE_APP_SRC_IMU_K230_IIM42622_CPP
#define DART_INCLUDE_FIRMWARE_APP_SRC_IMU_K230_IIM42622_CPP

#include <expected>

#include "imu/interface.cpp"
#include "spi/iim42622.h"

namespace dart::imu::k230 {

class Iim42622 final : public IImu {
public:
    explicit Iim42622(const dart_iim42622_config_t& config) noexcept
        : config_(config) {}
    ~Iim42622() override;

    Iim42622(const Iim42622&) = delete;
    Iim42622& operator=(const Iim42622&) = delete;
    Iim42622(Iim42622&&) = delete;
    Iim42622& operator=(Iim42622&&) = delete;

    std::expected<void, ErrorCode> initialize() noexcept;
    std::expected<Sample, ErrorCode> read_sample() noexcept override;
    void deinitialize() noexcept;

private:
    dart_iim42622_config_t config_{};
    dart_iim42622_t* sensor_ = nullptr;

private:
    static ErrorCode convert_error(dart_iim42622_status_t status) noexcept {
        switch (status) {
        case DART_IIM42622_ALREADY_INITIALIZED: return ErrorCode::kAlreadyInitialized;
        case DART_IIM42622_NOT_INITIALIZED: return ErrorCode::kNotInitialized;
        case DART_IIM42622_NO_MEMORY: return ErrorCode::kNoMemory;
        case DART_IIM42622_UNEXPECTED_DEVICE_ID: return ErrorCode::kUnexpectedDeviceId;
        case DART_IIM42622_CLOCK_ERROR: return ErrorCode::kClock;
        case DART_IIM42622_INVALID_ARGUMENT: return ErrorCode::kInvalidConfiguration;
        case DART_IIM42622_BUS_ERROR:
        case DART_IIM42622_OK: return ErrorCode::kBusTransfer;
        }
        return ErrorCode::kBusTransfer;
    }
};

inline Iim42622::~Iim42622() { deinitialize(); }

inline std::expected<void, ErrorCode> Iim42622::initialize() noexcept {
    const auto status = dart_iim42622_init(&config_, &sensor_);
    if (status != DART_IIM42622_OK)
        return std::unexpected(convert_error(status));
    return {};
}

inline std::expected<Sample, ErrorCode> Iim42622::read_sample() noexcept {
    dart_iim42622_sample_t c_sample{};
    const auto status = dart_iim42622_read_sample(sensor_, &c_sample);
    if (status != DART_IIM42622_OK)
        return std::unexpected(convert_error(status));

    Sample sample{};
    sample.raw_counts.accelerometer = {
        c_sample.accelerometer[0], c_sample.accelerometer[1], c_sample.accelerometer[2]};
    sample.raw_counts.gyroscope = {
        c_sample.gyroscope[0], c_sample.gyroscope[1], c_sample.gyroscope[2]};
    sample.acceleration_m_s2 = {
        c_sample.acceleration_m_s2[0], c_sample.acceleration_m_s2[1],
        c_sample.acceleration_m_s2[2]};
    sample.angular_velocity_rad_s = {
        c_sample.angular_velocity_rad_s[0], c_sample.angular_velocity_rad_s[1],
        c_sample.angular_velocity_rad_s[2]};
    sample.temperature_c = c_sample.temperature_c;
    sample.timestamp_ns = c_sample.timestamp_ns;
    sample.flags = c_sample.flags;
    return sample;
}

inline void Iim42622::deinitialize() noexcept { dart_iim42622_deinit(&sensor_); }

} // namespace dart::imu::k230

#endif // DART_INCLUDE_FIRMWARE_APP_SRC_IMU_K230_IIM42622_CPP
