#ifndef DART_DETECTION_DETECTOR_CPP
#define DART_DETECTION_DETECTOR_CPP

// Include this file for the detection classes; definitions stay with the classes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "camera/camera.hpp"

namespace dart::detection {

struct Point {
    float x = 0.0F;
    float y = 0.0F;
};

// Full-image coordinates; right and bottom edges are exclusive.
struct Rect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

enum class DetectionError : int {
    kInvalidConfig = 1,
    kNoMemory,
    kNotPrepared,
    kInvalidFrame,
    kInvalidWindow,
    kWorkspaceExhausted,
    kOutOfOrderFrame,
};

struct Target {
    bool valid = false;
    Point center{};
    Rect bounds{};
    std::uint32_t area = 0;
    float radius = 0.0F;
    float quality = 0.0F;
    bool clipped = false;
};

struct Blob {
    Point center{};
    Rect bounds{};
    std::uint32_t area = 0;
    float fill = 0.0F;
    float circularity = 0.0F;
    float angle = 0.0F;
    float length = 0.0F;
    float thickness = 0.0F;
    bool clipped = false;
};

inline constexpr std::uint32_t kMaxCandidates = 256;

// v1 defaults are a starting point, UNVALIDATED on v2 hardware.
// Y8 carries no color: the guide light is detected by brightness and shape.
struct LightConfig {
    // GREEN_STEP is used only while acquiring a light on a full-frame scan;
    // locked tracking scans use step 1 to keep ROI geometry stable.
    std::uint32_t min_area = 3;
    std::uint32_t far_area_threshold = 30;
    std::uint32_t far_size_px = 20;
    std::uint32_t green_step = 2;
    std::uint32_t confirm_frames = 2;
    std::uint32_t far_confirm_frames = 3;
    float far_jump_k = 2.5F;
    std::uint32_t far_jump_min_px = 10;
    std::uint32_t deadband_px = 2;
    float min_fill = 0.15F;
    float min_circularity = 0.0F;
    float circularity_weight = 1.0F;
    float aspect_min = 0.15F;
    float aspect_max = 6.0F;
    float max_area_fraction = 0.25F;
};

struct ArmorConfig {
    std::uint32_t length_floor = 2;
    std::uint32_t area_floor = 2;
    float length_min_k = 0.2F;
    float length_max_k = 8.0F;
    float tiny_px = 20.0F;
    float aspect_min = 2.0F;
    float fill_min = 0.5F;
    float flat_deg = 12.0F;
    float parallel_deg = 15.0F;
    float thickness_ratio = 0.3F;
    float length_ratio = 0.4F;
    float overlap = 0.4F;
    float gap_max_k = 10.0F;
    float diagonal_min_sin = 0.30F;
    float border_penalty = 30.0F;
    float window_width_k = 3.0F;
    float window_height_k = 2.0F;
    float window_gap_k = 0.0F;
    float window_min_side = 24.0F;
    float near_k = 8.0F;
    float near_min_px = 32.0F;
    float span_min_k = 0.2F;
    float span_max_k = 12.0F;
    std::uint32_t plate_up_lo_pct = 20;
    std::uint32_t plate_up_hi_pct = 400;
    std::uint32_t plate_offx_pct = 200;
    float association_k = 0.6F;
    float association_min_px = 4.0F;
    std::uint32_t track_hold = 2;
};

namespace detail {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kDegrees = 180.0F / static_cast<float>(kPi);

inline bool positive(float value) noexcept { return std::isfinite(value) && value > 0.0F; }
inline bool fraction(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}
inline std::uint64_t squares(std::uint64_t n) noexcept { return n * (n + 1) * (2 * n + 1) / 6; }

inline Target target_from(const Blob& blob) noexcept {
    return {true, blob.center, blob.bounds, blob.area,
            std::sqrt(static_cast<float>(blob.area) / static_cast<float>(kPi)),
            blob.clipped ? 0.25F : 1.0F, blob.clipped};
}

struct Bar {
    const Blob* blob = nullptr;
    Point center{};
    Point axis{};
    float length = 0.0F;
    float thickness = 0.0F;
};

inline float dot(Point a, Point b) noexcept { return a.x * b.x + a.y * b.y; }
inline float cross(Point a, Point b) noexcept { return a.x * b.y - a.y * b.x; }
inline Point delta(Point a, Point b) noexcept { return {a.x - b.x, a.y - b.y}; }

inline bool plate_anchor_ok(const ArmorConfig& config, Point center, const Target& light) noexcept {
    const float scale = static_cast<float>(std::max(light.bounds.width, light.bounds.height));
    if (!(scale > 0.0F)) return false;
    const float up = light.center.y - center.y;
    const float off_x = std::fabs(center.x - light.center.x);
    return up * 100.0F >= config.plate_up_lo_pct * scale &&
           up * 100.0F <= config.plate_up_hi_pct * scale &&
           off_x * 100.0F <= config.plate_offx_pct * scale;
}

inline bool make_bar(const ArmorConfig& config, const Blob& blob, float scale, Bar& bar,
                     bool tracking = false) noexcept {
    const float minimum = std::max(static_cast<float>(config.length_floor),
                                   std::floor(config.length_min_k * scale));
    const float maximum = std::max(minimum, std::floor(config.length_max_k * scale));
    if (blob.area < std::max(static_cast<float>(config.area_floor), std::floor(minimum * 0.5F)))
        return false;
    bar.blob = &blob;
    bar.center = {blob.bounds.x + (blob.bounds.width - 1) * 0.5F,
                  blob.bounds.y + (blob.bounds.height - 1) * 0.5F};
    float angle = blob.angle;
    bar.length = blob.length;
    bar.thickness = blob.thickness;
    if (!(bar.length > 0.0F && bar.thickness > 0.0F)) {
        bar.length = static_cast<float>(std::max(blob.bounds.width, blob.bounds.height));
        bar.thickness = static_cast<float>(std::min(blob.bounds.width, blob.bounds.height));
        angle = blob.bounds.height >= blob.bounds.width ? static_cast<float>(kPi * 0.5) : 0.0F;
    }
    // Remote blobs are too quantized for shape tests; temporal confirmation in roi.cpp
    // is the gate. Keep only the area/pixel floor above.
    const bool far = std::max(blob.bounds.width, blob.bounds.height) < config.tiny_px;
    if (!tracking && !far && (bar.length < minimum || bar.length > maximum)) return false;
    const float degrees = std::fabs(angle) * kDegrees;
    const float skew = std::min(degrees, std::fabs(90.0F - degrees));
    if (!tracking && !far) {
        if (bar.length < config.aspect_min * bar.thickness) return false;
        if (skew <= config.flat_deg && blob.fill < config.fill_min) return false;
    }
    bar.axis = {std::cos(angle), std::sin(angle)};
    return true;
}

inline void ends(const ArmorConfig& config, const Bar& bar, Point& first, Point& last) noexcept {
    Point axis = bar.axis;
    float length = bar.length;
    // Fix endpoint order across the +/- pi/2 discontinuity of the principal axis.
    const bool vertical = std::fabs(axis.y) >= std::fabs(axis.x);
    if ((vertical && axis.y < 0.0F) || (!vertical && axis.x < 0.0F))
        axis = {-axis.x, -axis.y};
    const float skew = std::atan2(std::min(std::fabs(axis.x), std::fabs(axis.y)),
                                  std::max(std::fabs(axis.x), std::fabs(axis.y))) * kDegrees;
    if (skew <= config.flat_deg) {
        axis = vertical ? Point{0.0F, 1.0F} : Point{1.0F, 0.0F};
        length = static_cast<float>((vertical ? bar.blob->bounds.height : bar.blob->bounds.width) - 1);
    }
    first = {bar.center.x - axis.x * length * 0.5F, bar.center.y - axis.y * length * 0.5F};
    last = {bar.center.x + axis.x * length * 0.5F, bar.center.y + axis.y * length * 0.5F};
}

inline bool armor_center(const ArmorConfig& config, const Bar& a, const Bar& b, Point& out) noexcept {
    Point a0, a1, b0, b1;
    ends(config, a, a0, a1);
    ends(config, b, b0, b1);
    // Noisy axes can straddle the vertical/horizontal convention at 45 degrees.
    // Align endpoint order before constructing the crossed diagonals.
    if (dot(delta(a1, a0), delta(b1, b0)) < 0.0F) std::swap(b0, b1);
    const Point u = delta(b1, a0), v = delta(b0, a1), offset = delta(a1, a0);
    const double determinant = cross(u, v);
    const double lengths = std::sqrt(static_cast<double>(dot(u, u)) * dot(v, v));
    if (lengths <= 0.0 || std::fabs(determinant) < config.diagonal_min_sin * lengths) return false;
    const double t = cross(offset, v) / determinant;
    const double s = cross(offset, u) / determinant;
    if (t < 0.0 || t > 1.0 || s < 0.0 || s > 1.0) return false;
    out = {static_cast<float>(a0.x + t * u.x), static_cast<float>(a0.y + t * u.y)};
    return std::isfinite(out.x) && std::isfinite(out.y);
}

inline Rect unite_bounds(Rect a, Rect b) noexcept {
    const auto x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.width, b.x + b.width) - x,
            std::max(a.y + a.height, b.y + b.height) - y};
}

}  // namespace detail

// Startup allocation only. scan() borrows FrameView synchronously and returns
// candidates valid until the next scan()/prepare(); pixels are never modified.
class BlobWorkspace {
public:
    BlobWorkspace() noexcept = default;
    ~BlobWorkspace() = default;
    BlobWorkspace(const BlobWorkspace&) = delete;
    BlobWorkspace& operator=(const BlobWorkspace&) = delete;

    std::expected<void, DetectionError> prepare(
        std::uint32_t max_width, std::uint32_t max_height,
        std::uint32_t max_runs = 16384, std::uint32_t max_candidates = kMaxCandidates) noexcept {
        if (max_width == 0 || max_height == 0 || max_width > 65535 || max_height > 65535 ||
            max_runs == 0 || max_runs > 65535 || max_candidates == 0 ||
            max_candidates > kMaxCandidates || max_candidates > max_runs)
            return std::unexpected(DetectionError::kInvalidConfig);
        auto runs = std::unique_ptr<Run[]>(new (std::nothrow) Run[max_runs]);
        auto components = std::unique_ptr<Component[]>(new (std::nothrow) Component[max_runs]);
        auto candidates = std::unique_ptr<Blob[]>(new (std::nothrow) Blob[max_candidates]);
        if (!runs || !components || !candidates) return std::unexpected(DetectionError::kNoMemory);
        runs_ = std::move(runs);
        components_ = std::move(components);
        candidates_ = std::move(candidates);
        max_width_ = max_width;
        max_height_ = max_height;
        max_runs_ = max_runs;
        max_candidates_ = max_candidates;
        run_count_ = run_high_water_ = 0;
        return {};
    }
    std::expected<void, DetectionError> validate(const camera::FrameView& frame) const noexcept {
        if (!runs_) return std::unexpected(DetectionError::kNotPrepared);
        if (frame.width == 0 || frame.height == 0 || frame.width > max_width_ ||
            frame.height > max_height_ || frame.stride < frame.width || frame.luma.data() == nullptr)
            return std::unexpected(DetectionError::kInvalidFrame);
        const std::uint64_t required = static_cast<std::uint64_t>(frame.height - 1) * frame.stride + frame.width;
        if (required > frame.luma.size()) return std::unexpected(DetectionError::kInvalidFrame);
        return {};
    }
    std::expected<std::span<const Blob>, DetectionError> scan(
        const camera::FrameView& frame, Rect window, std::uint8_t threshold,
        std::uint32_t min_area = 2, std::uint32_t sample_step = 1) noexcept {
        auto valid = validate(frame);
        if (!valid) return std::unexpected(valid.error());
        if (window.width == 0 || window.height == 0 || window.x >= frame.width ||
            window.y >= frame.height || window.width > frame.width - window.x ||
            window.height > frame.height - window.y)
            return std::unexpected(DetectionError::kInvalidWindow);
        sample_step = std::clamp(sample_step, 1U, 8U);
        run_count_ = 0;
        std::uint32_t previous_begin = 0, previous_end = 0;
        std::uint32_t sampled_y = 0;
        for (std::uint32_t y = 0; y < window.height; y += sample_step, ++sampled_y) {
            const auto* row = frame.luma.data() + static_cast<std::size_t>(window.y + y) * frame.stride + window.x;
            const auto current_begin = run_count_;
            auto previous = previous_begin;
            std::uint32_t x = 0;
            while (x < window.width) {
                if (std::to_integer<std::uint8_t>(row[x]) < threshold) { x += sample_step; continue; }
                const auto start = x;
                x += sample_step;
                while (x < window.width && std::to_integer<std::uint8_t>(row[x]) >= threshold) x += sample_step;
                if (run_count_ == max_runs_) return std::unexpected(DetectionError::kWorkspaceExhausted);
                const auto id = run_count_++;
                runs_[id] = {start / sample_step, (x - sample_step) / sample_step,
                             sampled_y, id, 0};
                run_high_water_ = std::max(run_high_water_, run_count_);
                const auto sampled_start = start / sample_step;
                const auto sampled_end = (x - sample_step) / sample_step;
                while (previous < previous_end && runs_[previous].x1 + 1 < sampled_start) ++previous;
                for (auto k = previous; k < previous_end && runs_[k].x0 <= sampled_end + 1; ++k) unite(id, k);
            }
            previous_begin = current_begin;
            previous_end = run_count_;
        }
        std::fill_n(components_.get(), run_count_, Component{});
        for (std::uint32_t i = 0; i < run_count_; ++i) {
            const auto& run = runs_[i];
            auto& component = components_[root(i)];
            const std::uint64_t length = run.x1 - run.x0 + 1;
            const std::uint64_t sx = (static_cast<std::uint64_t>(run.x0) + run.x1) * length / 2;
            if (component.area == 0) {
                component.x0 = run.x0; component.x1 = run.x1;
                component.y0 = component.y1 = run.y;
            } else {
                component.x0 = std::min(component.x0, run.x0);
                component.x1 = std::max(component.x1, run.x1);
                component.y1 = std::max(component.y1, run.y);
            }
            component.area += length;
            component.sx += sx;
            component.sy += length * run.y;
            component.sxx += detail::squares(run.x1) - (run.x0 > 0 ? detail::squares(run.x0 - 1) : 0);
            component.syy += length * run.y * run.y;
            component.sxy += sx * run.y;
        }
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < run_count_; ++i) {
            const auto& c = components_[i];
            if (c.area == 0) continue;
            // Convert sampled points back to physical pixels for far-area and radius gates.
            const std::uint64_t physical_area = c.area * sample_step * sample_step;
            if (physical_area < min_area) continue;
            if (count == max_candidates_) return std::unexpected(DetectionError::kWorkspaceExhausted);
            const double area = static_cast<double>(c.area), mx = c.sx / area, my = c.sy / area;
            const double xx = std::max(0.0, c.sxx / area - mx * mx);
            const double yy = std::max(0.0, c.syy / area - my * my);
            const double xy = c.sxy / area - mx * my;
            const double spread = std::hypot(xx - yy, 2.0 * xy);
            const double major = std::max(0.0, (xx + yy + spread) * 0.5);
            const double minor = std::max(0.0, (xx + yy - spread) * 0.5);
            auto& blob = candidates_[count++];
            const auto bbox_x = c.x0 * sample_step;
            const auto bbox_y = c.y0 * sample_step;
            const auto physical_width = std::min(window.width - std::min(window.width, bbox_x),
                                                 (c.x1 - c.x0 + 1) * sample_step);
            const auto physical_height = std::min(window.height - std::min(window.height, bbox_y),
                                                  (c.y1 - c.y0 + 1) * sample_step);
            blob.bounds = {window.x + bbox_x, window.y + bbox_y, physical_width, physical_height};
            blob.center = {static_cast<float>(window.x + mx * sample_step),
                           static_cast<float>(window.y + my * sample_step)};
            blob.area = static_cast<std::uint32_t>(std::min<std::uint64_t>(physical_area,
                                                                            std::numeric_limits<std::uint32_t>::max()));
            blob.fill = static_cast<float>(static_cast<double>(physical_area) /
                                           (static_cast<double>(blob.bounds.width) * blob.bounds.height));
            const double elongation = major > 1.0e-9 ? std::sqrt(minor / major) : 0.6;
            blob.circularity = static_cast<float>(elongation * std::min(1.0, blob.fill / (detail::kPi * 0.25)));
            blob.angle = static_cast<float>(0.5 * std::atan2(2.0 * xy, xx - yy));
            blob.length = static_cast<float>(std::sqrt(12.0 * major) * sample_step);
            blob.thickness = static_cast<float>(std::sqrt(12.0 * minor) * sample_step);
            blob.clipped = c.x0 == 0 || c.y0 == 0 ||
                           c.x1 + 1 >= (window.width + sample_step - 1) / sample_step ||
                           c.y1 + 1 >= (window.height + sample_step - 1) / sample_step;
        }
        return std::span<const Blob>(candidates_.get(), count);
    }

    std::uint32_t runs() const noexcept { return run_count_; }
    std::uint32_t run_high_water() const noexcept { return run_high_water_; }

private:
    struct Run {
        std::uint32_t x0 = 0, x1 = 0, y = 0, parent = 0, rank = 0;
    };
    struct Component {
        std::uint64_t area = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        std::uint32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    };
    std::uint32_t root(std::uint32_t id) noexcept {
        while (runs_[id].parent != id) {
            runs_[id].parent = runs_[runs_[id].parent].parent;
            id = runs_[id].parent;
        }
        return id;
    }
    void unite(std::uint32_t a, std::uint32_t b) noexcept {
        a = root(a);
        b = root(b);
        if (a == b) return;
        if (runs_[a].rank < runs_[b].rank) std::swap(a, b);
        runs_[b].parent = a;
        if (runs_[a].rank == runs_[b].rank) ++runs_[a].rank;
    }

    std::unique_ptr<Run[]> runs_;
    std::unique_ptr<Component[]> components_;
    std::unique_ptr<Blob[]> candidates_;
    std::uint32_t max_width_ = 0;
    std::uint32_t max_height_ = 0;
    std::uint32_t max_runs_ = 0;
    std::uint32_t max_candidates_ = 0;
    std::uint32_t run_count_ = 0;
    std::uint32_t run_high_water_ = 0;
};

class light_detect {
public:
    explicit light_detect(LightConfig config = {}) noexcept : config_(config) {}
    bool valid_config() const noexcept {
        return config_.min_area > 0 && config_.far_area_threshold > 0 && config_.far_size_px > 0 &&
               config_.green_step > 0 && config_.confirm_frames > 0 && config_.far_confirm_frames > 0 &&
               config_.far_jump_min_px > 0 && std::isfinite(config_.far_jump_k) && config_.far_jump_k > 0.0F &&
               detail::fraction(config_.min_fill) && detail::fraction(config_.min_circularity) &&
               std::isfinite(config_.circularity_weight) && config_.circularity_weight >= 0.0F &&
               detail::positive(config_.aspect_min) && detail::positive(config_.aspect_max) &&
               config_.aspect_min <= config_.aspect_max && detail::positive(config_.max_area_fraction) &&
               config_.max_area_fraction <= 1.0F;
    }
    Target detect(std::span<const Blob> blobs, std::uint64_t frame_area) const noexcept {
        const Blob* best = nullptr;
        float best_score = -1.0F;
        for (const auto& blob : blobs) {
            if (blob.bounds.width == 0 || blob.bounds.height == 0) continue;
            const float aspect = static_cast<float>(blob.bounds.width) / blob.bounds.height;
            const bool far = blob.area < config_.far_area_threshold ||
                             std::max(blob.bounds.width, blob.bounds.height) < config_.far_size_px;
            if (blob.area < config_.min_area ||
                blob.area > frame_area * config_.max_area_fraction ||
                (!far && (blob.fill < config_.min_fill ||
                          blob.circularity < config_.min_circularity ||
                          aspect < config_.aspect_min || aspect > config_.aspect_max)))
                continue;
            const float roundness = std::max(0.05F, blob.circularity);
            const float weight = config_.circularity_weight;
            const float shape = weight == 0.0F ? 1.0F : weight == 1.0F ? roundness : std::pow(roundness, weight);
            const float score = blob.area * shape;
            if (score > best_score) { best = &blob; best_score = score; }
        }
        return best ? detail::target_from(*best) : Target{};
    }

private:
    LightConfig config_;
};

class armor_detect {
public:
    explicit armor_detect(ArmorConfig config = {}) noexcept : config_(config) {}
    bool valid_config() const noexcept {
        const std::array values{config_.length_min_k, config_.length_max_k, config_.tiny_px,
            config_.aspect_min, config_.gap_max_k, config_.diagonal_min_sin,
            config_.window_width_k, config_.window_height_k, config_.window_min_side,
            config_.near_k, config_.near_min_px, config_.span_min_k, config_.span_max_k,
            config_.association_k, config_.association_min_px};
        return std::all_of(values.begin(), values.end(), detail::positive) && config_.length_floor > 0 &&
               config_.area_floor > 0 && config_.length_min_k <= config_.length_max_k &&
               config_.span_min_k <= config_.span_max_k && config_.diagonal_min_sin <= 1.0F &&
               config_.plate_up_lo_pct <= config_.plate_up_hi_pct && config_.plate_offx_pct > 0 &&
               config_.track_hold > 0 &&
               detail::fraction(config_.thickness_ratio) && detail::fraction(config_.length_ratio) && detail::fraction(config_.overlap) &&
               std::isfinite(config_.window_gap_k) && config_.window_gap_k >= 0.0F &&
               std::isfinite(config_.border_penalty) && config_.border_penalty >= 0.0F &&
               std::isfinite(config_.flat_deg) && config_.flat_deg >= 0.0F && config_.flat_deg <= 45.0F &&
               detail::positive(config_.parallel_deg) && config_.parallel_deg < 90.0F;
    }
    // Both detectors read the same candidates. As in v1, the current light
    // anchors the armor search above it. No held target is emitted on a miss.
    Target detect(
        std::span<const Blob> blobs, const Target& light,
        const Target* previous = nullptr) const noexcept {
        if (!light.valid || !detail::positive(light.radius) || blobs.size() > kMaxCandidates) return {};
        // Candidate coordinates come from a validated workspace (at most 65535).
        const Rect window = search_window(light, 65535, 65535);
        const float scale = 2.0F * light.radius;
        std::array<detail::Bar, kMaxCandidates> bars{};
        std::size_t count = 0;
        for (const auto& blob : blobs) {
            if (blob.bounds.width == 0 || blob.bounds.height == 0 ||
                blob.center.x < window.x || blob.center.x >= window.x + window.width ||
                blob.center.y < window.y || blob.center.y >= window.y + window.height) continue;
            if (detail::make_bar(config_, blob, scale, bars[count], previous && previous->valid)) ++count;
        }
        Target best{}, associated{};
        float best_score = std::numeric_limits<float>::infinity(), associated_score = best_score;
        const float parallel = std::cos(config_.parallel_deg / detail::kDegrees);
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t j = i + 1; j < count; ++j) {
                const auto& a = bars[i]; const auto& b = bars[j];
                if (std::fabs(detail::dot(a.axis, b.axis)) < parallel) continue;
                const float longest = std::max(a.length, b.length), shortest = std::min(a.length, b.length);
                if (shortest < config_.length_ratio * longest ||
                    std::min(a.thickness, b.thickness) < config_.thickness_ratio * std::max(a.thickness, b.thickness)) continue;
                const Point offset = detail::delta(b.center, a.center);
                const float separation = std::fabs(detail::cross(offset, a.axis));
                const float along = std::fabs(detail::dot(offset, a.axis));
                if ((a.length + b.length) * 0.5F - along < config_.overlap * shortest ||
                    separation <= (a.thickness + b.thickness) * 0.5F ||
                    separation > config_.gap_max_k * longest || separation < config_.span_min_k * scale ||
                    separation > config_.span_max_k * scale) continue;
                Point center;
                if (!detail::armor_center(config_, a, b, center)) continue;
                if (!detail::plate_anchor_ok(config_, center, light)) continue;
                const float score = ((longest - shortest) + along) * 100.0F / longest +
                                    config_.border_penalty * (a.blob->clipped + b.blob->clipped);
                const Rect bounds = detail::unite_bounds(a.blob->bounds, b.blob->bounds);
                const std::uint32_t area = a.blob->area + b.blob->area;
                const bool clipped = a.blob->clipped || b.blob->clipped;
                const Target target{true, center, bounds, area,
                    std::sqrt(area / static_cast<float>(detail::kPi)), clipped ? 0.25F : 1.0F, clipped};
                if (score < best_score || (score == best_score && area > best.area)) {
                    best = target; best_score = score;
                }
                if (previous && previous->valid) {
                    const Point change = detail::delta(center, previous->center);
                    const float tolerance = std::max(config_.association_min_px, config_.association_k * longest);
                    const float old_size = static_cast<float>(std::max(previous->bounds.width, previous->bounds.height));
                    const float size = static_cast<float>(std::max(bounds.width, bounds.height));
                    if (detail::dot(change, change) <= tolerance * tolerance && size >= old_size * 0.5F &&
                        size <= old_size * 2.0F && score < associated_score) {
                        associated = target; associated_score = score;
                    }
                }
            }
        }
        return associated.valid ? associated : best;
    }
    Rect search_window(const Target& light, std::uint32_t width, std::uint32_t height) const noexcept {
        if (!light.valid || !detail::positive(light.radius) || !std::isfinite(light.center.x) ||
            !std::isfinite(light.center.y)) return {};
        const double scale = 2.0 * light.radius;
        const double w = std::max<double>(config_.window_min_side, config_.window_width_k * scale);
        const double h = std::max<double>(config_.window_min_side, config_.window_height_k * scale);
        const double x = light.center.x - w * 0.5;
        const double y = light.center.y - light.radius - config_.window_gap_k * scale - h;
        const double x0 = std::clamp(std::floor(x), 0.0, static_cast<double>(width));
        const double y0 = std::clamp(std::floor(y), 0.0, static_cast<double>(height));
        const double x1 = std::clamp(std::ceil(x + w), x0, static_cast<double>(width));
        const double y1 = std::clamp(std::ceil(y + h), y0, static_cast<double>(height));
        return {static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
                static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)};
    }

private:
    ArmorConfig config_;
};

}  // namespace dart::detection

#endif  // DART_DETECTION_DETECTOR_CPP
