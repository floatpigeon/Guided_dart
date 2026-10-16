#ifndef DART_SRC_DETECTOR_ROI_CPP
#define DART_SRC_DETECTOR_ROI_CPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>

#include "armor.cpp"
#include "blob_workspace.cpp"
#include "geometry.cpp"
#include "interface.cpp"
#include "light.cpp"

namespace dart::detection {

class RoiDetector final : public IDetector {
public:
    struct Config {
        struct Window {
            std::uint32_t min_width = 24;
            std::uint32_t min_height = 64;
            float size_k = 2.6F;
            float margin = 12.0F;
            float grow_per_miss = 0.5F;
            float full_area_fraction = 0.75F;
            std::uint32_t reset_after_misses = 3;
            std::uint32_t full_scan_interval = 0;
            std::chrono::milliseconds max_gap{250};
        };
        std::uint8_t threshold = 50;
        BlobWorkspace::Config workspace{};
        LightDetector::Config light{};
        ArmorDetector::Config armor{};
        Window roi{};
    };
    RoiDetector() noexcept;
    explicit RoiDetector(Config config) noexcept;
    RoiDetector(const RoiDetector&) = delete;
    RoiDetector& operator=(const RoiDetector&) = delete;

    std::expected<void, DetectionError>
        Prepare(std::uint32_t max_width, std::uint32_t max_height) noexcept;
    std::expected<DetectResult, DetectionError>
        Detect(const camera::FrameView& frame) noexcept override;
    void Reset() noexcept override;
    std::uint32_t run_high_water() const noexcept { return workspace_.run_high_water(); }

private:
    struct RoiState {
        Rect envelope{};
        std::uint32_t missed_frames = 0;
        std::uint32_t frames_since_full = 0;
    };

    bool ValidRoiConfig() const noexcept;
    Rect NextWindow(std::uint32_t width, std::uint32_t height) const noexcept;

    struct FrameHistory {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint64_t sequence = 0;
        std::chrono::microseconds received{};
        bool valid = false;
    };

    struct TargetHistory {
        Target light{};
        Target armor{};
        std::uint32_t armor_misses = 0;
    };

    std::expected<void, DetectionError>
        Measure(const camera::FrameView& frame, Rect window, DetectResult& result) noexcept;
    void UpdateTracking(DetectResult& result, std::uint32_t width, std::uint32_t height) noexcept;
    void ResetTracking() noexcept;

    Config config_;
    LightDetector light_;
    ArmorDetector armor_;
    BlobWorkspace workspace_;
    LightTracker light_tracker_;
    TargetHistory targets_{};
    FrameHistory frame_{};
    RoiState roi_state_{};
};

inline RoiDetector::RoiDetector() noexcept : RoiDetector(Config{}) {}

inline RoiDetector::RoiDetector(Config config) noexcept
    : config_(config)
    , light_(config.light)
    , armor_(config.armor)
    , light_tracker_(config.light) {}

inline std::expected<void, DetectionError>
    RoiDetector::Prepare(std::uint32_t max_width, std::uint32_t max_height) noexcept {
    if (!light_.ValidConfig() || !light_tracker_.ValidConfig() || !armor_.ValidConfig()
        || !ValidRoiConfig() || config_.threshold == 0) {
        return std::unexpected(DetectionError::kInvalidConfig);
    }
    auto ready = workspace_.Prepare(max_width, max_height, config_.workspace);
    if (!ready)
        return ready;
    Reset();
    return {};
}

inline std::expected<DetectResult, DetectionError>
    RoiDetector::Detect(const camera::FrameView& frame) noexcept {
    auto valid = workspace_.Validate(frame);
    if (!valid)
        return std::unexpected(valid.error());
    if (frame_.valid && (frame.sequence <= frame_.sequence || frame.received < frame_.received)) {
        return std::unexpected(DetectionError::kOutOfOrderFrame);
    }
    if (frame_.valid
        && (frame.width != frame_.width || frame.height != frame_.height
            || frame.received - frame_.received > config_.roi.max_gap)) {
        ResetTracking();
    }
    DetectResult result{};
    result.source = {
        frame.sequence, frame.source_pts, frame.received, frame.source_pts_timebase_validated};
    const Rect window = NextWindow(frame.width, frame.height);
    auto measured = Measure(frame, window, result);
    if (!measured)
        return std::unexpected(measured.error());
    if (!result.scan.full_frame
        && (!result.light.valid || result.light.clipped || result.armor.clipped)) {
        result.scan.rescanned = true;
        measured = Measure(frame, FullWindow(frame.width, frame.height), result);
        if (!measured)
            return std::unexpected(measured.error());
    }
    UpdateTracking(result, frame.width, frame.height);
    frame_ = {frame.width, frame.height, frame.sequence, frame.received, true};
    return result;
}

inline void RoiDetector::ResetTracking() noexcept {
    targets_ = {};
    roi_state_ = {};
    light_tracker_.Reset();
}

inline void RoiDetector::Reset() noexcept {
    ResetTracking();
    frame_ = {};
}

inline void RoiDetector::UpdateTracking(
    DetectResult& result, std::uint32_t width, std::uint32_t height) noexcept {
    if (result.target.valid) {
        targets_.light = result.light;
        if (result.armor.valid) {
            targets_.armor = result.armor;
            targets_.armor_misses = 0;
        } else if (targets_.armor.valid) {
            IncrementSaturated(targets_.armor_misses);
            if (targets_.armor_misses >= config_.armor.track_hold)
                targets_.armor = {};
        }
        roi_state_.envelope = UniteWindows(
            result.light.bounds, armor_.SearchWindow(result.light, width, height));
        if (result.armor.valid) {
            roi_state_.envelope =
                UniteWindows(roi_state_.envelope, result.armor.bounds);
        }
        roi_state_.missed_frames = 0;
    } else {
        // An unconfirmed but valid far light keeps contributing to confirmation.
        // Only an actual miss clears the streak and unlocks the light.
        if (!light_tracker_.has_measurement())
            light_tracker_.ClearConfirmation();
        IncrementSaturated(roi_state_.missed_frames);
        if (roi_state_.missed_frames >= config_.roi.reset_after_misses
            && !light_tracker_.has_measurement()) {
            targets_ = {};
            roi_state_.envelope = {};
            light_tracker_.ClearConfirmation();
        }
    }
    result.missed_frames = roi_state_.missed_frames;
    if (result.scan.full_frame) {
        roi_state_.frames_since_full = 0;
    } else {
        IncrementSaturated(roi_state_.frames_since_full);
    }
}

inline std::expected<void, DetectionError> RoiDetector::Measure(
    const camera::FrameView& frame, Rect window, DetectResult& result) noexcept {
    const bool sampled_acquire =
        IsFullWindow(window, frame.width, frame.height) && config_.light.search_step > 1;
    auto blobs = workspace_.Scan(
        frame, window, config_.threshold,
        std::min(config_.light.min_area, config_.armor.area_floor),
        sampled_acquire ? config_.light.search_step : 1);
    if (!blobs)
        return std::unexpected(blobs.error());
    result.scan.candidates = static_cast<std::uint32_t>(blobs->size());
    result.scan.search_window = window;
    result.scan.full_frame = IsFullWindow(window, frame.width, frame.height);
    result.light = light_.Detect(*blobs, static_cast<std::uint64_t>(frame.width) * frame.height);
    if (!light_tracker_.Confirm(result.light, result.source.frame_id, targets_.light)) {
        result.light = {};
        result.armor = {};
    } else if (sampled_acquire) {
        // Coarse full-frame acquisition is followed by a full-resolution scan of
        // just the light-anchored armor window.
        const Rect armor_window = armor_.SearchWindow(result.light, frame.width, frame.height);
        if (armor_window.width == 0 || armor_window.height == 0) {
            result.armor = {};
        } else {
            auto armor_blobs = workspace_.Scan(
                frame, armor_window, config_.threshold,
                std::min(config_.light.min_area, config_.armor.area_floor), 1);
            if (!armor_blobs)
                return std::unexpected(armor_blobs.error());
            result.scan.candidates += static_cast<std::uint32_t>(armor_blobs->size());
            result.armor = armor_.Detect(*armor_blobs, result.light, &targets_.armor);
        }
    } else {
        result.armor = armor_.Detect(*blobs, result.light, &targets_.armor);
    }
    result.target = result.armor.valid ? result.armor : result.light;
    result.kind = result.armor.valid ? TargetKind::kArmor
                : result.light.valid ? TargetKind::kLight
                                     : TargetKind::kNone;
    return {};
}

inline bool RoiDetector::ValidRoiConfig() const noexcept {
    const auto& config = config_.roi;
    return config.min_width > 0 && config.min_height > 0 && std::isfinite(config.size_k)
        && config.size_k >= 1.0F && std::isfinite(config.margin) && config.margin >= 0.0F
        && std::isfinite(config.grow_per_miss) && config.grow_per_miss >= 0.0F
        && std::isfinite(config.full_area_fraction) && config.full_area_fraction > 0.0F
        && config.full_area_fraction <= 1.0F && config.reset_after_misses > 0
        && config.max_gap.count() > 0;
}

inline Rect RoiDetector::NextWindow(std::uint32_t width, std::uint32_t height) const noexcept {
    const auto& config = config_.roi;
    const auto& state = roi_state_;
    const bool has_light = targets_.light.valid;
    if (!has_light || state.missed_frames >= config.reset_after_misses
        || (config.full_scan_interval > 0 && state.frames_since_full >= config.full_scan_interval)) {
        return FullWindow(width, height);
    }
    const double grow = 1.0 + static_cast<double>(config.grow_per_miss) * state.missed_frames;
    const double rw = std::min<double>(
        width, std::max<double>(
                   config.min_width, (state.envelope.width * static_cast<double>(config.size_k)
                                      + 2.0 * config.margin)
                                         * grow));
    const double rh = std::min<double>(
        height, std::max<double>(
                    config.min_height, (state.envelope.height * static_cast<double>(config.size_k)
                                        + 2.0 * config.margin)
                                           * grow));
    const auto w = static_cast<std::uint32_t>(std::ceil(rw));
    const auto h = static_cast<std::uint32_t>(std::ceil(rh));
    if (static_cast<double>(w) * h
        >= static_cast<double>(width) * height * config.full_area_fraction) {
        return FullWindow(width, height);
    }
    // Translate the whole window into the frame, preserving its size at edges.
    const double cx = state.envelope.x + state.envelope.width * 0.5;
    const double cy = state.envelope.y + state.envelope.height * 0.5;
    const auto x = static_cast<std::uint32_t>(
        std::clamp(std::floor(cx - w * 0.5), 0.0, static_cast<double>(width - w)));
    const auto y = static_cast<std::uint32_t>(
        std::clamp(std::floor(cy - h * 0.5), 0.0, static_cast<double>(height - h)));
    return {x, y, w, h};
}

} // namespace dart::detection

#endif // DART_SRC_DETECTOR_ROI_CPP
