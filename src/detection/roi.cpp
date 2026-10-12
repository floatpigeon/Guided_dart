#ifndef DART_DETECTION_ROI_CPP
#define DART_DETECTION_ROI_CPP

#include "src/detection/detector.cpp"

#include <chrono>

namespace dart::detection {

enum class TargetKind : std::uint8_t { kNone, kLight, kArmor };

struct DetectResult {
    TargetKind kind = TargetKind::kNone;
    Target target{};  // Armor center when available, otherwise light center.
    Target light{};
    Target armor{};
    Rect search_window{};
    std::uint64_t source_frame_id = 0;
    std::uint64_t source_pts = 0;
    std::chrono::microseconds received{};
    bool source_pts_timebase_validated = false;
    bool full_frame = false;
    bool rescanned = false;
    std::uint32_t candidates = 0;
    std::uint32_t missed_frames = 0;
};

class IDetector {
public:
    virtual ~IDetector() = default;
    virtual std::expected<DetectResult, DetectionError> detect(const camera::FrameView& frame) noexcept = 0;
    virtual void reset() noexcept = 0;
};

// Initial window policy; UNVALIDATED on v2 hardware.
struct RoiConfig {
    std::uint32_t min_width = 64;
    std::uint32_t min_height = 64;
    float size_k = 1.5F;
    float margin = 12.0F;
    float grow_per_miss = 0.5F;
    float full_area_fraction = 0.75F;
    std::uint32_t reset_after_misses = 3;
    std::uint32_t full_scan_interval = 0;
    std::chrono::milliseconds max_gap{250};
};

struct DetectionConfig {
    std::uint8_t threshold = 50;
    std::uint32_t max_runs = 16384;
    std::uint32_t max_candidates = kMaxCandidates;
    LightConfig light{};
    ArmorConfig armor{};
    RoiConfig roi{};
};

namespace detail {

inline Rect full_window(std::uint32_t width, std::uint32_t height) noexcept { return {0, 0, width, height}; }
inline bool is_full(Rect window, const camera::FrameView& frame) noexcept {
    return window.x == 0 && window.y == 0 && window.width == frame.width && window.height == frame.height;
}
inline Rect unite(Rect a, Rect b) noexcept {
    if (b.width == 0 || b.height == 0) return a;
    const auto x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.width, b.x + b.width) - x,
            std::max(a.y + a.height, b.y + b.height) - y};
}

}  // namespace detail

// One instance per ordered capture pipeline. prepare() allocates at startup.
// detect(lease.view()) borrows pixels only during the call; the pipeline owns
// and releases the lease. Results and state never keep frame pointers.
class roi final : public IDetector {
public:
    explicit roi(DetectionConfig config = {}) noexcept : config_(config), light_(config.light), armor_(config.armor) {}
    std::expected<void, DetectionError> prepare(std::uint32_t max_width, std::uint32_t max_height) noexcept {
        const auto& c = config_.roi;
        if (!light_.valid_config() || !armor_.valid_config() || config_.threshold == 0 ||
            c.min_width == 0 || c.min_height == 0 || !std::isfinite(c.size_k) || c.size_k < 1.0F ||
            !std::isfinite(c.margin) || c.margin < 0.0F || !std::isfinite(c.grow_per_miss) ||
            c.grow_per_miss < 0.0F || !std::isfinite(c.full_area_fraction) || c.full_area_fraction <= 0.0F ||
            c.full_area_fraction > 1.0F || c.reset_after_misses == 0 || c.max_gap.count() <= 0)
            return std::unexpected(DetectionError::kInvalidConfig);
        auto ready = workspace_.prepare(max_width, max_height, config_.max_runs, config_.max_candidates);
        if (!ready) return ready;
        reset();
        return {};
    }
    std::expected<DetectResult, DetectionError> detect(const camera::FrameView& frame) noexcept override {
        auto valid = workspace_.validate(frame);
        if (!valid) return std::unexpected(valid.error());
        if (have_frame_ && (frame.sequence <= last_sequence_ || frame.received < last_received_))
            return std::unexpected(DetectionError::kOutOfOrderFrame);
        if (have_frame_ && (frame.width != last_width_ || frame.height != last_height_ ||
            frame.received - last_received_ > config_.roi.max_gap)) {
            previous_light_ = previous_armor_ = {};
            armor_track_misses_ = 0;
            last_light_measure_ = {};
            light_hits_ = 0;
            last_confirm_sequence_ = 0;
            confirm_seen_ = false;
            last_confirm_input_valid_ = false;
            last_measurement_valid_ = false;
            light_locked_ = false;
            envelope_ = {};
            missed_ = since_full_ = 0;
        }
        DetectResult result{};
        result.source_frame_id = frame.sequence;
        result.source_pts = frame.source_pts;
        result.received = frame.received;
        result.source_pts_timebase_validated = frame.source_pts_timebase_validated;
        const Rect window = next_window(frame.width, frame.height);
        auto measured = measure(frame, window, result);
        if (!measured) return std::unexpected(measured.error());
        if (!result.full_frame && (!result.light.valid || result.light.clipped || result.armor.clipped)) {
            result.rescanned = true;
            measured = measure(frame, detail::full_window(frame.width, frame.height), result);
            if (!measured) return std::unexpected(measured.error());
        }
        if (result.target.valid) {
            previous_light_ = result.light;
            if (result.armor.valid) {
                previous_armor_ = result.armor;
                armor_track_misses_ = 0;
            } else if (previous_armor_.valid && armor_track_misses_ < std::numeric_limits<std::uint32_t>::max()) {
                ++armor_track_misses_;
                if (armor_track_misses_ >= config_.armor.track_hold) previous_armor_ = {};
            }
            envelope_ = detail::unite(result.light.bounds, armor_.search_window(result.light, frame.width, frame.height));
            if (result.armor.valid) envelope_ = detail::unite(envelope_, result.armor.bounds);
            missed_ = 0;
        } else {
            // A valid but not-yet-confirmed far light must keep contributing to
            // FAR_CONFIRM. Only an actual miss clears the confirmation streak.
            if (!last_measurement_valid_) {
                last_light_measure_ = {};
                light_hits_ = 0;
                light_locked_ = false;
            }
            if (missed_ < std::numeric_limits<std::uint32_t>::max()) ++missed_;
            if (missed_ >= config_.roi.reset_after_misses && !last_measurement_valid_) {
                previous_light_ = previous_armor_ = {};
                armor_track_misses_ = 0;
                envelope_ = {};
                last_light_measure_ = {};
                light_hits_ = 0;
                light_locked_ = false;
            }
        }
        result.missed_frames = missed_;
        if (result.full_frame) since_full_ = 0;
        else if (since_full_ < std::numeric_limits<std::uint32_t>::max()) ++since_full_;
        last_width_ = frame.width;
        last_height_ = frame.height;
        last_sequence_ = frame.sequence;
        last_received_ = frame.received;
        have_frame_ = true;
        return result;
    }
    void reset() noexcept override {
        previous_light_ = previous_armor_ = {};
        armor_track_misses_ = 0;
        last_light_measure_ = {};
        envelope_ = {};
        missed_ = since_full_ = last_width_ = last_height_ = 0;
        last_sequence_ = 0;
        light_hits_ = 0;
        last_confirm_sequence_ = 0;
        confirm_seen_ = false;
        last_confirm_input_valid_ = false;
        last_measurement_valid_ = false;
        light_locked_ = false;
        last_received_ = {};
        have_frame_ = false;
    }
    std::uint32_t run_high_water() const noexcept { return workspace_.run_high_water(); }

private:
    Rect next_window(std::uint32_t width, std::uint32_t height) const noexcept {
        if (!previous_light_.valid || missed_ >= config_.roi.reset_after_misses ||
            (config_.roi.full_scan_interval > 0 && since_full_ >= config_.roi.full_scan_interval))
            return detail::full_window(width, height);
        const double grow = 1.0 + static_cast<double>(config_.roi.grow_per_miss) * missed_;
        const double rw = std::min<double>(width, std::max<double>(config_.roi.min_width,
            (envelope_.width * static_cast<double>(config_.roi.size_k) + 2.0 * config_.roi.margin) * grow));
        const double rh = std::min<double>(height, std::max<double>(config_.roi.min_height,
            (envelope_.height * static_cast<double>(config_.roi.size_k) + 2.0 * config_.roi.margin) * grow));
        const auto w = static_cast<std::uint32_t>(std::ceil(rw)), h = static_cast<std::uint32_t>(std::ceil(rh));
        if (static_cast<double>(w) * h >= static_cast<double>(width) * height * config_.roi.full_area_fraction)
            return detail::full_window(width, height);
        // Translate the whole window into the frame, preserving its size at edges.
        const double cx = envelope_.x + envelope_.width * 0.5, cy = envelope_.y + envelope_.height * 0.5;
        const auto x = static_cast<std::uint32_t>(std::clamp(std::floor(cx - w * 0.5), 0.0, static_cast<double>(width - w)));
        const auto y = static_cast<std::uint32_t>(std::clamp(std::floor(cy - h * 0.5), 0.0, static_cast<double>(height - h)));
        return {x, y, w, h};
    }
    bool confirm_light(DetectResult& out) noexcept {
        const bool same_frame = confirm_seen_ && out.source_frame_id == last_confirm_sequence_;
        // A ROI miss followed by the same-frame full-frame rescan is allowed to
        // supply the first valid observation; two valid scans of one frame count
        // only once.
        if (same_frame && (last_confirm_input_valid_ || !out.light.valid)) return light_locked_;
        last_confirm_sequence_ = out.source_frame_id;
        confirm_seen_ = true;
        last_confirm_input_valid_ = out.light.valid;
        if (!out.light.valid) {
            light_hits_ = 0;
            last_light_measure_ = {};
            return false;
        }
        const bool far = out.light.area < config_.light.far_area_threshold ||
                         std::max(out.light.bounds.width, out.light.bounds.height) < config_.light.far_size_px;
        if (far && last_light_measure_.valid) {
            const float jump = std::max(static_cast<float>(config_.light.far_jump_min_px),
                                        config_.light.far_jump_k *
                                        static_cast<float>(std::max(last_light_measure_.bounds.width,
                                                                     last_light_measure_.bounds.height)));
            const float dx = out.light.center.x - last_light_measure_.center.x;
            const float dy = out.light.center.y - last_light_measure_.center.y;
            if (std::fabs(dx) > jump || std::fabs(dy) > jump) light_hits_ = 0;
        }
        last_light_measure_ = out.light;
        if (light_hits_ < std::numeric_limits<std::uint32_t>::max()) ++light_hits_;
        const auto needed = far ? config_.light.far_confirm_frames : config_.light.confirm_frames;
        if (!light_locked_ && light_hits_ >= needed) light_locked_ = true;
        if (!light_locked_) return false;
        if (config_.light.deadband_px > 0 && previous_light_.valid &&
            std::fabs(out.light.center.x - previous_light_.center.x) <= config_.light.deadband_px &&
            std::fabs(out.light.center.y - previous_light_.center.y) <= config_.light.deadband_px)
            out.light = previous_light_;
        return true;
    }
    std::expected<void, DetectionError> measure(const camera::FrameView& frame, Rect window, DetectResult& out) noexcept {
        const bool sampled_acquire = detail::is_full(window, frame) && config_.light.green_step > 1;
        auto blobs = workspace_.scan(frame, window, config_.threshold,
            std::min(config_.light.min_area, config_.armor.area_floor),
            sampled_acquire ? config_.light.green_step : 1);
        if (!blobs) return std::unexpected(blobs.error());
        out.candidates = static_cast<std::uint32_t>(blobs->size());
        out.search_window = window;
        out.full_frame = detail::is_full(window, frame);
        out.light = light_.detect(*blobs, static_cast<std::uint64_t>(frame.width) * frame.height);
        last_measurement_valid_ = out.light.valid;
        if (!confirm_light(out)) {
            out.light = {};
            out.armor = {};
        } else if (sampled_acquire) {
            // Keep coarse full-frame light acquisition, then inspect only the
            // light-anchored armor window at full resolution.
            const Rect armor_window = armor_.search_window(out.light, frame.width, frame.height);
            if (armor_window.width == 0 || armor_window.height == 0) {
                out.armor = {};
            } else {
                auto armor_blobs = workspace_.scan(frame, armor_window, config_.threshold,
                    std::min(config_.light.min_area, config_.armor.area_floor), 1);
                if (!armor_blobs) return std::unexpected(armor_blobs.error());
                out.candidates += static_cast<std::uint32_t>(armor_blobs->size());
                out.armor = armor_.detect(*armor_blobs, out.light, &previous_armor_);
            }
        } else {
            out.armor = armor_.detect(*blobs, out.light, &previous_armor_);
        }
        out.target = out.armor.valid ? out.armor : out.light;
        out.kind = out.armor.valid ? TargetKind::kArmor : out.light.valid ? TargetKind::kLight : TargetKind::kNone;
        return {};
    }

    DetectionConfig config_;
    light_detect light_;
    armor_detect armor_;
    BlobWorkspace workspace_;
    Target previous_light_{};
    Target last_light_measure_{};
    Target previous_armor_{};
    std::uint32_t armor_track_misses_ = 0;
    std::uint32_t light_hits_ = 0;
    std::uint64_t last_confirm_sequence_ = 0;
    bool confirm_seen_ = false;
    bool last_confirm_input_valid_ = false;
    bool last_measurement_valid_ = false;
    bool light_locked_ = false;
    Rect envelope_{};
    std::uint32_t missed_ = 0;
    std::uint32_t since_full_ = 0;
    std::uint32_t last_width_ = 0;
    std::uint32_t last_height_ = 0;
    std::uint64_t last_sequence_ = 0;
    std::chrono::microseconds last_received_{};
    bool have_frame_ = false;
};

}  // namespace dart::detection

#endif  // DART_DETECTION_ROI_CPP
