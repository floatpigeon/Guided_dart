#include "detector.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>

#include "camera/camera.hpp"

namespace dart::detection {
namespace {

Rect FullWindow(std::uint32_t width, std::uint32_t height) noexcept {
    return {0, 0, width, height};
}

bool IsFullWindow(Rect window, const camera::FrameView& frame) noexcept {
    return window.x == 0 && window.y == 0 && window.width == frame.width
        && window.height == frame.height;
}

Rect UniteWindows(Rect a, Rect b) noexcept {
    if (b.width == 0 || b.height == 0)
        return a;
    const auto x = std::min(a.x, b.x);
    const auto y = std::min(a.y, b.y);
    return {
        x, y, std::max(a.x + a.width, b.x + b.width) - x,
        std::max(a.y + a.height, b.y + b.height) - y};
}

void IncrementSaturated(std::uint32_t& value) noexcept {
    if (value < std::numeric_limits<std::uint32_t>::max())
        ++value;
}

} // namespace

namespace internal {

LightTracker::LightTracker(LightConfig config) noexcept
    : config_(config) {}

bool LightTracker::ValidConfig() const noexcept {
    return config_.confirm_frames > 0 && config_.far_confirm_frames > 0
        && config_.far_jump_min_px > 0 && std::isfinite(config_.far_jump_k)
        && config_.far_jump_k > 0.0F;
}

bool LightTracker::Confirm(Target& light, std::uint64_t frame_id, const Target& previous) noexcept {
    state_.measurement_valid = light.valid;
    const bool same_frame = state_.seen_frame && frame_id == state_.last_frame_id;
    // A same-frame rescan may supply the first valid observation, but two valid
    // scans of one frame count only once toward temporal confirmation.
    if (same_frame && (state_.input_valid || !light.valid))
        return state_.locked;
    state_.last_frame_id = frame_id;
    state_.seen_frame = true;
    state_.input_valid = light.valid;
    if (!light.valid) {
        state_.hits = 0;
        state_.last_measurement = {};
        return false;
    }
    const bool far = light.area < config_.far_area_threshold
                  || std::max(light.bounds.width, light.bounds.height) < config_.far_size_px;
    if (far && state_.last_measurement.valid) {
        const float jump = std::max(
            static_cast<float>(config_.far_jump_min_px),
            config_.far_jump_k
                * static_cast<float>(std::max(
                    state_.last_measurement.bounds.width, state_.last_measurement.bounds.height)));
        const float dx = light.center.x - state_.last_measurement.center.x;
        const float dy = light.center.y - state_.last_measurement.center.y;
        if (std::fabs(dx) > jump || std::fabs(dy) > jump)
            state_.hits = 0;
    }
    state_.last_measurement = light;
    IncrementSaturated(state_.hits);
    const auto needed = far ? config_.far_confirm_frames : config_.confirm_frames;
    if (!state_.locked && state_.hits >= needed)
        state_.locked = true;
    if (!state_.locked)
        return false;
    if (config_.deadband_px > 0 && previous.valid
        && std::fabs(light.center.x - previous.center.x) <= static_cast<float>(config_.deadband_px)
        && std::fabs(light.center.y - previous.center.y)
               <= static_cast<float>(config_.deadband_px)) {
        light = previous;
    }
    return true;
}

void LightTracker::ClearConfirmation() noexcept {
    state_.last_measurement = {};
    state_.hits = 0;
    state_.locked = false;
}

void LightTracker::Reset() noexcept { state_ = {}; }

RoiPolicy::RoiPolicy(RoiConfig config) noexcept
    : config_(config) {}

bool RoiPolicy::ValidConfig() const noexcept {
    return config_.min_width > 0 && config_.min_height > 0 && std::isfinite(config_.size_k)
        && config_.size_k >= 1.0F && std::isfinite(config_.margin) && config_.margin >= 0.0F
        && std::isfinite(config_.grow_per_miss) && config_.grow_per_miss >= 0.0F
        && std::isfinite(config_.full_area_fraction) && config_.full_area_fraction > 0.0F
        && config_.full_area_fraction <= 1.0F && config_.reset_after_misses > 0
        && config_.max_gap.count() > 0;
}

Rect RoiPolicy::NextWindow(
    std::uint32_t width, std::uint32_t height, bool has_light,
    const RoiState& state) const noexcept {
    if (!has_light || state.missed_frames >= config_.reset_after_misses
        || (config_.full_scan_interval > 0
            && state.frames_since_full >= config_.full_scan_interval)) {
        return FullWindow(width, height);
    }
    const double grow = 1.0 + static_cast<double>(config_.grow_per_miss) * state.missed_frames;
    const double rw = std::min<double>(
        width, std::max<double>(
                   config_.min_width, (state.envelope.width * static_cast<double>(config_.size_k)
                                       + 2.0 * config_.margin)
                                          * grow));
    const double rh = std::min<double>(
        height, std::max<double>(
                    config_.min_height, (state.envelope.height * static_cast<double>(config_.size_k)
                                         + 2.0 * config_.margin)
                                            * grow));
    const auto w = static_cast<std::uint32_t>(std::ceil(rw));
    const auto h = static_cast<std::uint32_t>(std::ceil(rh));
    if (static_cast<double>(w) * h
        >= static_cast<double>(width) * height * config_.full_area_fraction) {
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

} // namespace internal

RoiDetector::RoiDetector(DetectionConfig config) noexcept
    : config_(config)
    , light_(config.light)
    , armor_(config.armor)
    , light_tracker_(config.light)
    , roi_policy_(config.roi) {}

std::expected<void, DetectionError>
    RoiDetector::Prepare(std::uint32_t max_width, std::uint32_t max_height) noexcept {
    if (!light_.ValidConfig() || !light_tracker_.ValidConfig() || !armor_.ValidConfig()
        || !roi_policy_.ValidConfig() || config_.threshold == 0) {
        return std::unexpected(DetectionError::kInvalidConfig);
    }
    auto ready = workspace_.Prepare(max_width, max_height, config_.workspace);
    if (!ready)
        return ready;
    Reset();
    return {};
}

std::expected<DetectResult, DetectionError>
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
    const Rect window =
        roi_policy_.NextWindow(frame.width, frame.height, targets_.light.valid, roi_state_);
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

void RoiDetector::ResetTracking() noexcept {
    targets_ = {};
    roi_state_ = {};
    light_tracker_.Reset();
}

void RoiDetector::Reset() noexcept {
    ResetTracking();
    frame_ = {};
}

void RoiDetector::UpdateTracking(
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
        roi_state_.envelope =
            UniteWindows(result.light.bounds, armor_.SearchWindow(result.light, width, height));
        if (result.armor.valid) {
            roi_state_.envelope = UniteWindows(roi_state_.envelope, result.armor.bounds);
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

std::expected<void, DetectionError> RoiDetector::Measure(
    const camera::FrameView& frame, Rect window, DetectResult& result) noexcept {
    const bool sampled_acquire = IsFullWindow(window, frame) && config_.light.search_step > 1;
    auto blobs = workspace_.Scan(
        frame, window, config_.threshold,
        std::min(config_.light.min_area, config_.armor.area_floor),
        sampled_acquire ? config_.light.search_step : 1);
    if (!blobs)
        return std::unexpected(blobs.error());
    result.scan.candidates = static_cast<std::uint32_t>(blobs->size());
    result.scan.search_window = window;
    result.scan.full_frame = IsFullWindow(window, frame);
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

} // namespace dart::detection
