#ifndef DART_IMU_K230_IIM42622_HPP
#define DART_IMU_K230_IIM42622_HPP

#include "imu/imu_sample.hpp"
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
};

/* Naming used by the k230_v1 hardware layer; keep it as a source-compatible
 * type alias while the concrete implementation remains scoped to this adapter. */
using IIM42622Driver = Iim42622;

}  // namespace dart::imu::k230

#endif
