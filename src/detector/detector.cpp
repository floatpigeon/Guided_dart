#include "detector.hpp"

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
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kDegrees = 180.0F / static_cast<float>(kPi);

bool IsPositive(float value) noexcept { return std::isfinite(value) && value > 0.0F; }
bool IsFraction(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}
std::uint64_t SumSquares(std::uint64_t n) noexcept { return n * (n + 1) * (2 * n + 1) / 6; }

Target TargetFromBlob(const Blob& blob) noexcept {
    return {
        true,
        blob.center,
        blob.bounds,
        blob.area,
        std::sqrt(static_cast<float>(blob.area) / static_cast<float>(kPi)),
        blob.clipped ? 0.25F : 1.0F,
        blob.clipped};
}

struct Bar {
    const Blob* blob = nullptr;
    Point center{};
    Point axis{};
    float length = 0.0F;
    float thickness = 0.0F;
};

float Dot(Point a, Point b) noexcept { return a.x * b.x + a.y * b.y; }
float Cross(Point a, Point b) noexcept { return a.x * b.y - a.y * b.x; }
Point Delta(Point a, Point b) noexcept { return {a.x - b.x, a.y - b.y}; }

bool PlateAnchorOk(const ArmorConfig& config, Point center, const Target& light) noexcept {
    const float scale = static_cast<float>(std::max(light.bounds.width, light.bounds.height));
    if (!(scale > 0.0F))
        return false;
    const float up = light.center.y - center.y;
    const float off_x = std::fabs(center.x - light.center.x);
    return up * 100.0F >= static_cast<float>(config.plate_up_lo_pct) * scale
        && up * 100.0F <= static_cast<float>(config.plate_up_hi_pct) * scale
        && off_x * 100.0F <= static_cast<float>(config.plate_offx_pct) * scale;
}

bool MakeBar(
    const ArmorConfig& config, const Blob& blob, float scale, Bar& bar,
    bool tracking = false) noexcept {
    const float minimum =
        std::max(static_cast<float>(config.length_floor), std::floor(config.length_min_k * scale));
    const float maximum = std::max(minimum, std::floor(config.length_max_k * scale));
    if (static_cast<float>(blob.area)
        < std::max(static_cast<float>(config.area_floor), std::floor(minimum * 0.5F)))
        return false;
    bar.blob = &blob;
    bar.center = {
        static_cast<float>(blob.bounds.x) + static_cast<float>((blob.bounds.width - 1)) * 0.5F,
        static_cast<float>(blob.bounds.y) + static_cast<float>((blob.bounds.height - 1)) * 0.5F};
    float angle = blob.angle;
    bar.length = blob.length;
    bar.thickness = blob.thickness;
    if (!(bar.length > 0.0F && bar.thickness > 0.0F)) {
        bar.length = static_cast<float>(std::max(blob.bounds.width, blob.bounds.height));
        bar.thickness = static_cast<float>(std::min(blob.bounds.width, blob.bounds.height));
        angle = blob.bounds.height >= blob.bounds.width ? static_cast<float>(kPi * 0.5) : 0.0F;
    }
    // Remote blobs are too quantized for shape tests; LightTracker's temporal
    // confirmation is the gate. Keep only the area/pixel floor above.
    const bool far = static_cast<float>(std::max(blob.bounds.width, blob.bounds.height))
                   < static_cast<float>(config.tiny_px);
    if (!tracking && !far && (bar.length < minimum || bar.length > maximum))
        return false;
    const float degrees = std::fabs(angle) * kDegrees;
    const float skew = std::min(degrees, std::fabs(90.0F - degrees));
    if (!tracking && !far) {
        if (bar.length < config.aspect_min * bar.thickness)
            return false;
        if (skew <= config.flat_deg && blob.fill < config.fill_min)
            return false;
    }
    bar.axis = {std::cos(angle), std::sin(angle)};
    return true;
}

void BarEndpoints(const ArmorConfig& config, const Bar& bar, Point& first, Point& last) noexcept {
    Point axis = bar.axis;
    float length = bar.length;
    // Fix endpoint order across the +/- pi/2 discontinuity of the principal axis.
    const bool vertical = std::fabs(axis.y) >= std::fabs(axis.x);
    if ((vertical && axis.y < 0.0F) || (!vertical && axis.x < 0.0F))
        axis = {-axis.x, -axis.y};
    const float skew = std::atan2(
                           std::min(std::fabs(axis.x), std::fabs(axis.y)),
                           std::max(std::fabs(axis.x), std::fabs(axis.y)))
                     * kDegrees;
    if (skew <= config.flat_deg) {
        axis = vertical ? Point{0.0F, 1.0F} : Point{1.0F, 0.0F};
        length =
            static_cast<float>((vertical ? bar.blob->bounds.height : bar.blob->bounds.width) - 1);
    }
    first = {bar.center.x - axis.x * length * 0.5F, bar.center.y - axis.y * length * 0.5F};
    last = {bar.center.x + axis.x * length * 0.5F, bar.center.y + axis.y * length * 0.5F};
}

bool ArmorCenter(const ArmorConfig& config, const Bar& a, const Bar& b, Point& out) noexcept {
    Point a0, a1, b0, b1;
    BarEndpoints(config, a, a0, a1);
    BarEndpoints(config, b, b0, b1);
    // Noisy axes can straddle the vertical/horizontal convention at 45 degrees.
    // Align endpoint order before constructing the crossed diagonals.
    if (Dot(Delta(a1, a0), Delta(b1, b0)) < 0.0F)
        std::swap(b0, b1);
    const Point u = Delta(b1, a0), v = Delta(b0, a1), offset = Delta(a1, a0);
    const double determinant = Cross(u, v);
    const double lengths = std::sqrt(static_cast<double>(Dot(u, u)) * Dot(v, v));
    if (lengths <= 0.0 || std::fabs(determinant) < config.diagonal_min_sin * lengths)
        return false;
    const double t = Cross(offset, v) / determinant;
    const double s = Cross(offset, u) / determinant;
    if (t < 0.0 || t > 1.0 || s < 0.0 || s > 1.0)
        return false;
    out = {static_cast<float>(a0.x + t * u.x), static_cast<float>(a0.y + t * u.y)};
    return std::isfinite(out.x) && std::isfinite(out.y);
}

Rect UniteBounds(Rect a, Rect b) noexcept {
    const auto x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {
        x, y, std::max(a.x + a.width, b.x + b.width) - x,
        std::max(a.y + a.height, b.y + b.height) - y};
}

} // namespace

// Scan storage is private to the connected-component extractor.
struct BlobWorkspace::Run {
    std::uint32_t x0 = 0;
    std::uint32_t x1 = 0;
    std::uint32_t y = 0;
    std::uint32_t parent = 0;
    std::uint32_t rank = 0;
};

struct BlobWorkspace::Component {
    std::uint64_t area = 0;
    std::uint64_t sx = 0;
    std::uint64_t sy = 0;
    std::uint64_t sxx = 0;
    std::uint64_t syy = 0;
    std::uint64_t sxy = 0;
    std::uint32_t x0 = 0;
    std::uint32_t x1 = 0;
    std::uint32_t y0 = 0;
    std::uint32_t y1 = 0;
};

BlobWorkspace::BlobWorkspace() noexcept = default;
BlobWorkspace::~BlobWorkspace() = default;

std::expected<void, DetectionError> BlobWorkspace::Prepare(
    std::uint32_t max_width, std::uint32_t max_height, WorkspaceConfig config) noexcept {
    const auto max_runs = config.max_runs;
    const auto max_candidates = config.max_candidates;
    if (max_width == 0 || max_height == 0 || max_width > 65535 || max_height > 65535
        || max_runs == 0 || max_runs > 65535 || max_candidates == 0
        || max_candidates > kMaxCandidates || max_candidates > max_runs)
        return std::unexpected(DetectionError::kInvalidConfig);
    auto runs = std::unique_ptr<Run[]>(new (std::nothrow) Run[max_runs]);
    auto components = std::unique_ptr<Component[]>(new (std::nothrow) Component[max_runs]);
    auto candidates = std::unique_ptr<Blob[]>(new (std::nothrow) Blob[max_candidates]);
    if (!runs || !components || !candidates)
        return std::unexpected(DetectionError::kNoMemory);
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

std::expected<void, DetectionError>
    BlobWorkspace::Validate(const camera::FrameView& frame) const noexcept {
    if (!runs_)
        return std::unexpected(DetectionError::kNotPrepared);
    if (frame.width == 0 || frame.height == 0 || frame.width > max_width_
        || frame.height > max_height_ || frame.stride < frame.width || frame.luma.data() == nullptr)
        return std::unexpected(DetectionError::kInvalidFrame);
    const std::uint64_t required =
        static_cast<std::uint64_t>(frame.height - 1) * frame.stride + frame.width;
    if (required > frame.luma.size())
        return std::unexpected(DetectionError::kInvalidFrame);
    return {};
}

std::expected<std::span<const Blob>, DetectionError> BlobWorkspace::Scan(
    const camera::FrameView& frame, Rect window, std::uint8_t threshold, std::uint32_t min_area,
    std::uint32_t sample_step) noexcept {
    auto valid = Validate(frame);
    if (!valid)
        return std::unexpected(valid.error());
    if (window.width == 0 || window.height == 0 || window.x >= frame.width
        || window.y >= frame.height || window.width > frame.width - window.x
        || window.height > frame.height - window.y)
        return std::unexpected(DetectionError::kInvalidWindow);
    sample_step = std::clamp(sample_step, 1U, 8U);
    run_count_ = 0;
    std::uint32_t previous_begin = 0, previous_end = 0;
    std::uint32_t sampled_y = 0;
    for (std::uint32_t y = 0; y < window.height; y += sample_step, ++sampled_y) {
        const auto* row =
            frame.luma.data() + static_cast<std::size_t>(window.y + y) * frame.stride + window.x;
        const auto current_begin = run_count_;
        auto previous = previous_begin;
        std::uint32_t x = 0;
        while (x < window.width) {
            if (std::to_integer<std::uint8_t>(row[x]) < threshold) {
                x += sample_step;
                continue;
            }
            const auto start = x;
            x += sample_step;
            while (x < window.width && std::to_integer<std::uint8_t>(row[x]) >= threshold)
                x += sample_step;
            if (run_count_ == max_runs_)
                return std::unexpected(DetectionError::kWorkspaceExhausted);
            const auto id = run_count_++;
            runs_[id] = {start / sample_step, (x - sample_step) / sample_step, sampled_y, id, 0};
            run_high_water_ = std::max(run_high_water_, run_count_);
            const auto sampled_start = start / sample_step;
            const auto sampled_end = (x - sample_step) / sample_step;
            while (previous < previous_end && runs_[previous].x1 + 1 < sampled_start)
                ++previous;
            for (auto k = previous; k < previous_end && runs_[k].x0 <= sampled_end + 1; ++k)
                Unite(id, k);
        }
        previous_begin = current_begin;
        previous_end = run_count_;
    }
    std::fill_n(components_.get(), run_count_, Component{});
    for (std::uint32_t i = 0; i < run_count_; ++i) {
        const auto& run = runs_[i];
        auto& component = components_[Root(i)];
        const std::uint64_t length = run.x1 - run.x0 + 1;
        const std::uint64_t sx = (static_cast<std::uint64_t>(run.x0) + run.x1) * length / 2;
        if (component.area == 0) {
            component.x0 = run.x0;
            component.x1 = run.x1;
            component.y0 = component.y1 = run.y;
        } else {
            component.x0 = std::min(component.x0, run.x0);
            component.x1 = std::max(component.x1, run.x1);
            component.y1 = std::max(component.y1, run.y);
        }
        component.area += length;
        component.sx += sx;
        component.sy += length * run.y;
        component.sxx += SumSquares(run.x1) - (run.x0 > 0 ? SumSquares(run.x0 - 1) : 0);
        component.syy += length * run.y * run.y;
        component.sxy += sx * run.y;
    }
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < run_count_; ++i) {
        const auto& c = components_[i];
        if (c.area == 0)
            continue;
        // Convert sampled points back to physical pixels for far-area and radius
        // gates.
        const std::uint64_t physical_area = c.area * sample_step * sample_step;
        if (physical_area < min_area)
            continue;
        if (count == max_candidates_)
            return std::unexpected(DetectionError::kWorkspaceExhausted);
        const double area = static_cast<double>(c.area), mx = static_cast<double>(c.sx) / area,
                     my = static_cast<double>(c.sy) / area;
        const double xx = std::max(0.0, static_cast<double>(c.sxx) / area - mx * mx);
        const double yy = std::max(0.0, static_cast<double>(c.syy) / area - my * my);
        const double xy = static_cast<double>(c.sxy) / area - mx * my;
        const double spread = std::hypot(xx - yy, 2.0 * xy);
        const double major = std::max(0.0, (xx + yy + spread) * 0.5);
        const double minor = std::max(0.0, (xx + yy - spread) * 0.5);
        auto& blob = candidates_[count++];
        const auto bbox_x = c.x0 * sample_step;
        const auto bbox_y = c.y0 * sample_step;
        const auto physical_width = std::min(
            window.width - std::min(window.width, bbox_x), (c.x1 - c.x0 + 1) * sample_step);
        const auto physical_height = std::min(
            window.height - std::min(window.height, bbox_y), (c.y1 - c.y0 + 1) * sample_step);
        blob.bounds = {window.x + bbox_x, window.y + bbox_y, physical_width, physical_height};
        blob.center = {
            static_cast<float>(window.x + mx * sample_step),
            static_cast<float>(window.y + my * sample_step)};
        blob.area = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(physical_area, std::numeric_limits<std::uint32_t>::max()));
        blob.fill = static_cast<float>(
            static_cast<double>(physical_area)
            / (static_cast<double>(blob.bounds.width) * blob.bounds.height));
        const double elongation = major > 1.0e-9 ? std::sqrt(minor / major) : 0.6;
        blob.circularity = static_cast<float>(elongation * std::min(1.0, blob.fill / (kPi * 0.25)));
        blob.angle = static_cast<float>(0.5 * std::atan2(2.0 * xy, xx - yy));
        blob.length = static_cast<float>(std::sqrt(12.0 * major) * sample_step);
        blob.thickness = static_cast<float>(std::sqrt(12.0 * minor) * sample_step);
        blob.clipped = c.x0 == 0 || c.y0 == 0
                    || c.x1 + 1 >= (window.width + sample_step - 1) / sample_step
                    || c.y1 + 1 >= (window.height + sample_step - 1) / sample_step;
    }
    return std::span<const Blob>(candidates_.get(), count);
}

std::uint32_t BlobWorkspace::Root(std::uint32_t id) noexcept {
    while (runs_[id].parent != id) {
        runs_[id].parent = runs_[runs_[id].parent].parent;
        id = runs_[id].parent;
    }
    return id;
}

void BlobWorkspace::Unite(std::uint32_t a, std::uint32_t b) noexcept {
    a = Root(a);
    b = Root(b);
    if (a == b)
        return;
    if (runs_[a].rank < runs_[b].rank)
        std::swap(a, b);
    runs_[b].parent = a;
    if (runs_[a].rank == runs_[b].rank)
        ++runs_[a].rank;
}

LightDetector::LightDetector(LightConfig config) noexcept
    : config_(config) {}

bool LightDetector::ValidConfig() const noexcept {
    return config_.min_area > 0 && config_.far_area_threshold > 0 && config_.far_size_px > 0
        && config_.search_step > 0 && IsFraction(config_.min_fill)
        && IsFraction(config_.min_circularity) && std::isfinite(config_.circularity_weight)
        && config_.circularity_weight >= 0.0F && IsPositive(config_.aspect_min)
        && IsPositive(config_.aspect_max) && config_.aspect_min <= config_.aspect_max
        && IsPositive(config_.max_area_fraction) && config_.max_area_fraction <= 1.0F;
}

Target LightDetector::Detect(std::span<const Blob> blobs, std::uint64_t frame_area) const noexcept {
    const Blob* best = nullptr;
    float best_score = -1.0F;
    for (const auto& blob : blobs) {
        if (blob.bounds.width == 0 || blob.bounds.height == 0)
            continue;
        const float aspect =
            static_cast<float>(blob.bounds.width) / static_cast<float>(blob.bounds.height);
        const bool far = blob.area < config_.far_area_threshold
                      || std::max(blob.bounds.width, blob.bounds.height) < config_.far_size_px;
        if (blob.area < config_.min_area
            || static_cast<float>(blob.area)
                   > static_cast<float>(frame_area) * static_cast<float>(config_.max_area_fraction)
            || (!far
                && (blob.fill < config_.min_fill || blob.circularity < config_.min_circularity
                    || aspect < config_.aspect_min || aspect > config_.aspect_max)))
            continue;
        const float roundness = std::max(0.05F, blob.circularity);
        const float weight = config_.circularity_weight;
        const float shape = weight == 0.0F ? 1.0F
                          : weight == 1.0F ? roundness
                                           : std::pow(roundness, weight);
        const float score = static_cast<float>(blob.area) * shape;
        if (score > best_score) {
            best = &blob;
            best_score = score;
        }
    }
    return best ? TargetFromBlob(*best) : Target{};
}

ArmorDetector::ArmorDetector(ArmorConfig config) noexcept
    : config_(config) {}

bool ArmorDetector::ValidConfig() const noexcept {
    const std::array values{
        config_.length_min_k,   config_.length_max_k,    config_.tiny_px,
        config_.aspect_min,     config_.gap_max_k,       config_.diagonal_min_sin,
        config_.window_width_k, config_.window_height_k, config_.window_min_side,
        config_.near_k,         config_.near_min_px,     config_.span_min_k,
        config_.span_max_k,     config_.association_k,   config_.association_min_px};
    return std::all_of(values.begin(), values.end(), IsPositive) && config_.length_floor > 0
        && config_.area_floor > 0 && config_.length_min_k <= config_.length_max_k
        && config_.span_min_k <= config_.span_max_k && config_.diagonal_min_sin <= 1.0F
        && config_.plate_up_lo_pct <= config_.plate_up_hi_pct && config_.plate_offx_pct > 0
        && config_.track_hold > 0 && IsFraction(config_.thickness_ratio)
        && IsFraction(config_.length_ratio) && IsFraction(config_.overlap)
        && std::isfinite(config_.window_gap_k) && config_.window_gap_k >= 0.0F
        && std::isfinite(config_.border_penalty) && config_.border_penalty >= 0.0F
        && std::isfinite(config_.flat_deg) && config_.flat_deg >= 0.0F && config_.flat_deg <= 45.0F
        && IsPositive(config_.parallel_deg) && config_.parallel_deg < 90.0F;
}

// Both selectors read measured candidates. The current light anchors the armor
// search above it; an old target is used only for association.
Target ArmorDetector::Detect(
    std::span<const Blob> blobs, const Target& light, const Target* previous) const noexcept {
    if (!light.valid || !IsPositive(light.radius) || blobs.size() > kMaxCandidates)
        return {};
    // Candidate coordinates come from a validated workspace (at most 65535).
    const Rect window = SearchWindow(light, 65535, 65535);
    const float scale = 2.0F * light.radius;
    std::array<Bar, kMaxCandidates> bars{};
    std::size_t count = 0;
    for (const auto& blob : blobs) {
        if (blob.bounds.width == 0 || blob.bounds.height == 0
            || blob.center.x < static_cast<float>(window.x)
            || blob.center.x >= static_cast<float>(window.x + window.width)
            || blob.center.y < static_cast<float>(window.y)
            || blob.center.y >= static_cast<float>(window.y + window.height))
            continue;
        if (MakeBar(config_, blob, scale, bars[count], previous && previous->valid))
            ++count;
    }
    Target best{}, associated{};
    float best_score = std::numeric_limits<float>::infinity(), associated_score = best_score;
    const float parallel = std::cos(config_.parallel_deg / kDegrees);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            const auto& a = bars[i];
            const auto& b = bars[j];
            if (std::fabs(Dot(a.axis, b.axis)) < parallel)
                continue;
            const float longest = std::max(a.length, b.length),
                        shortest = std::min(a.length, b.length);
            if (shortest < config_.length_ratio * longest
                || std::min(a.thickness, b.thickness)
                       < config_.thickness_ratio * std::max(a.thickness, b.thickness))
                continue;
            const Point offset = Delta(b.center, a.center);
            const float separation = std::fabs(Cross(offset, a.axis));
            const float along = std::fabs(Dot(offset, a.axis));
            if ((a.length + b.length) * 0.5F - along < config_.overlap * shortest
                || separation <= (a.thickness + b.thickness) * 0.5F
                || separation > config_.gap_max_k * longest
                || separation < config_.span_min_k * scale
                || separation > config_.span_max_k * scale)
                continue;
            Point center;
            if (!ArmorCenter(config_, a, b, center))
                continue;
            if (!PlateAnchorOk(config_, center, light))
                continue;
            const float score =
                ((longest - shortest) + along) * 100.0F / longest
                + static_cast<float>(config_.border_penalty)
                      * (static_cast<float>(a.blob->clipped) + static_cast<float>(b.blob->clipped));
            const Rect bounds = UniteBounds(a.blob->bounds, b.blob->bounds);
            const std::uint32_t area = a.blob->area + b.blob->area;
            const bool clipped = a.blob->clipped || b.blob->clipped;
            const Target target{
                true,
                center,
                bounds,
                area,
                std::sqrt(static_cast<float>(area) / static_cast<float>(kPi)),
                clipped ? 0.25F : 1.0F,
                clipped};
            if (score < best_score || (score == best_score && area > best.area)) {
                best = target;
                best_score = score;
            }
            if (previous && previous->valid) {
                const Point change = Delta(center, previous->center);
                const float tolerance =
                    std::max(config_.association_min_px, config_.association_k * longest);
                const float old_size =
                    static_cast<float>(std::max(previous->bounds.width, previous->bounds.height));
                const float size = static_cast<float>(std::max(bounds.width, bounds.height));
                if (Dot(change, change) <= tolerance * tolerance && size >= old_size * 0.5F
                    && size <= old_size * 2.0F && score < associated_score) {
                    associated = target;
                    associated_score = score;
                }
            }
        }
    }
    return associated.valid ? associated : best;
}

Rect ArmorDetector::SearchWindow(
    const Target& light, std::uint32_t width, std::uint32_t height) const noexcept {
    if (!light.valid || !IsPositive(light.radius) || !std::isfinite(light.center.x)
        || !std::isfinite(light.center.y))
        return {};
    const double scale = 2.0 * light.radius;
    const double w = std::max<double>(config_.window_min_side, config_.window_width_k * scale);
    const double h = std::max<double>(config_.window_min_side, config_.window_height_k * scale);
    const double x = light.center.x - w * 0.5;
    const double y = light.center.y - light.radius - config_.window_gap_k * scale - h;
    const double x0 = std::clamp(std::floor(x), 0.0, static_cast<double>(width));
    const double y0 = std::clamp(std::floor(y), 0.0, static_cast<double>(height));
    const double x1 = std::clamp(std::ceil(x + w), x0, static_cast<double>(width));
    const double y1 = std::clamp(std::ceil(y + h), y0, static_cast<double>(height));
    return {
        static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
        static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)};
}

} // namespace dart::detection
