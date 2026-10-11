#ifndef DART_K230_CAMERA_HPP
#define DART_K230_CAMERA_HPP

#include "camera.hpp"

struct dart_media;
struct dart_camera;

namespace dart::camera {

class K230Camera final : public ICamera {
public:
    static std::expected<std::unique_ptr<K230Camera>, ErrorPtr> create(
        const Config& config) noexcept;
    ~K230Camera() override;

    K230Camera(const K230Camera&) = delete;
    K230Camera& operator=(const K230Camera&) = delete;

    std::expected<void, ErrorPtr> start() noexcept override;
    std::expected<std::optional<FrameLease>, ErrorPtr> acquire(
        std::chrono::milliseconds timeout) noexcept override;
    std::expected<void, ErrorPtr> stop() noexcept override;
    State state() const noexcept override { return state_; }
    const Caps& caps() const noexcept override { return caps_; }
    std::expected<void, ErrorPtr> set_fixed_exposure_us(
        std::uint32_t us) noexcept override;

private:
    explicit K230Camera(const Config& config) noexcept : config_(config) {}

    static std::expected<void, ErrorPtr> release_lease(
        detail::FrameLeaseState* state) noexcept;
    static void remember_release_failure(detail::FrameLeaseState* state,
                                         ErrorPtr error) noexcept;

    std::expected<void, ErrorPtr> refresh_caps() noexcept;
    std::expected<void, ErrorPtr> refresh_state(Operation operation) const noexcept;
    std::expected<void, ErrorPtr> destroy_handles() noexcept;
    ErrorPtr pending_release_busy(Operation operation) const noexcept;
    bool has_pending_release() const noexcept { return pending_frame_.valid; }

    Config config_{};
    Caps caps_{};
    mutable State state_ = State::kEmpty;
    ::dart_media* media_ = nullptr;
    ::dart_camera* camera_ = nullptr;
    ErrorPtr pending_release_error_{};
    struct PendingFrame {
        const std::byte* data = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
        std::uint32_t flags = 0;
        std::uint64_t source_pts = 0;
        std::uint64_t received_us = 0;
        std::uint64_t token = 0;
        bool valid = false;
    } pending_frame_{};
    std::expected<void, ErrorPtr> retry_pending_release() noexcept;
};

}  // namespace dart::camera

#endif
