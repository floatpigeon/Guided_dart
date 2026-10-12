#ifndef DART_K230_CAMERA_HPP
#define DART_K230_CAMERA_HPP

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>

#include "camera/c_api/camera.h"
#include "camera/camera.hpp"

namespace dart::camera::k230 {

/* K230 implementation of ICamera.
 *
 * The lease capacity is a compile-time parameter injected by the caller. It sizes
 * the lease slots statically, so acquiring a frame allocates nothing, the bound is
 * visible to the compiler, and capacity() cannot drift from the storage the
 * adapter actually reserved. config.channel_buffers must be at least as large,
 * because every held lease keeps one driver buffer busy.
 */
template <std::uint32_t kCapacity>
class Camera final : public ICamera {
public:
    static_assert(kCapacity >= 1U, "a camera needs at least one frame lease slot");
    static_assert(kCapacity <= DART_CAMERA_MAX_OUTSTANDING_FRAMES,
                  "lease capacity exceeds what the C ABI can represent");

    static std::expected<std::unique_ptr<Camera>, ErrorPtr> create(
        const Config& config) noexcept;
    ~Camera() override;

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    std::expected<void, ErrorPtr> start() noexcept override;
    std::expected<AcquireResult, ErrorPtr> acquire(
        std::chrono::milliseconds timeout) noexcept override;
    std::expected<void, ErrorPtr> stop() noexcept override;
    State state() const noexcept override { return state_; }
    const Caps& caps() const noexcept override { return caps_; }
    std::expected<void, ErrorPtr> set_fixed_exposure_us(
        std::uint32_t us) noexcept override;
    std::uint32_t capacity() const noexcept override { return kCapacity; }
    std::uint32_t outstanding() const noexcept override;

private:
    explicit Camera(const Config& config) noexcept : config_(config) {}

    /* Trampoline stored in a lease slot, so a lease can release itself without
     * knowing which camera type owns the slot.
     */
    static std::expected<void, ErrorPtr> slot_release(
        detail::FrameLeaseSlot* slot) noexcept;

    std::expected<void, ErrorPtr> release_slot(detail::FrameLeaseSlot* slot) noexcept;
    std::expected<void, ErrorPtr> retry_pending_releases() noexcept;
    detail::FrameLeaseSlot* free_slot() noexcept;
    void fill_slot(detail::FrameLeaseSlot* slot,
                   const dart_camera_frame_t& frame) noexcept;
    void detach_leases() noexcept;

    std::expected<void, ErrorPtr> refresh_caps() noexcept;
    std::expected<void, ErrorPtr> refresh_state(Operation operation) const noexcept;
    std::expected<void, ErrorPtr> destroy_handles() noexcept;

    Config config_{};
    Caps caps_{};
    mutable State state_ = State::kEmpty;
    ::dart_media* media_ = nullptr;
    ::dart_camera* camera_ = nullptr;
    std::array<detail::FrameLeaseSlot, kCapacity> slots_{};
};

/* Factory: create -> init -> start stay separate, and a failed initialization
 * neither throws nor prints. The capacity parameter is the upper layer's
 * injection point; every capacity the C ABI allows is instantiated in
 * camera.cpp, so the call site needs no definition.
 */
template <std::uint32_t kCapacity = 1U>
std::expected<std::unique_ptr<ICamera>, ErrorPtr> create(
    const Config& config) noexcept {
    auto created = Camera<kCapacity>::create(config);
    if (!created) return std::unexpected<ErrorPtr>(std::move(created.error()));
    std::unique_ptr<ICamera> result(std::move(created.value()));
    return result;
}

}  // namespace dart::camera::k230

#endif
