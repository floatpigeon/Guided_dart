#ifndef DART_SRC_DETECTOR_BLOB_WORKSPACE_CPP
#define DART_SRC_DETECTOR_BLOB_WORKSPACE_CPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "geometry.cpp"
#include "interface.cpp"
#include "numeric.cpp"

namespace dart::detection {

class Blob {
public:
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

class BlobWorkspace {
public:
    struct Config {
        static constexpr std::uint32_t kMaxCandidates = 256;
        std::uint32_t max_runs = 16384;
        std::uint32_t max_candidates = kMaxCandidates;
    };
    BlobWorkspace() noexcept;
    ~BlobWorkspace();
    BlobWorkspace(const BlobWorkspace&) = delete;
    BlobWorkspace& operator=(const BlobWorkspace&) = delete;

    std::expected<void, DetectionError> Prepare(
        std::uint32_t max_width, std::uint32_t max_height, Config config) noexcept;
    std::expected<void, DetectionError> Prepare(
        std::uint32_t max_width, std::uint32_t max_height) noexcept {
        return Prepare(max_width, max_height, Config{});
    }
    std::expected<void, DetectionError> Validate(const camera::FrameView& frame) const noexcept;
    std::expected<std::span<const Blob>, DetectionError> Scan(
        const camera::FrameView& frame, Rect window, std::uint8_t threshold,
        std::uint32_t min_area = 2, std::uint32_t sample_step = 1) noexcept;

    std::uint32_t runs() const noexcept { return run_count_; }
    std::uint32_t run_high_water() const noexcept { return run_high_water_; }

private:
    struct Run {
        std::uint32_t x0 = 0;
        std::uint32_t x1 = 0;
        std::uint32_t y = 0;
        std::uint32_t parent = 0;
        std::uint32_t rank = 0;
    };

    struct Component {
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

    std::uint32_t Root(std::uint32_t id) noexcept;
    void Unite(std::uint32_t a, std::uint32_t b) noexcept;

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

inline BlobWorkspace::BlobWorkspace() noexcept = default;

inline BlobWorkspace::~BlobWorkspace() = default;

inline std::expected<void, DetectionError> BlobWorkspace::Prepare(
    std::uint32_t max_width, std::uint32_t max_height, Config config) noexcept {
    const auto max_runs = config.max_runs;
    const auto max_candidates = config.max_candidates;
    if (max_width == 0 || max_height == 0 || max_width > 65535 || max_height > 65535
        || max_runs == 0 || max_runs > 65535 || max_candidates == 0
        || max_candidates > Config::kMaxCandidates || max_candidates > max_runs)
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

inline std::expected<void, DetectionError>
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

inline std::expected<std::span<const Blob>, DetectionError> BlobWorkspace::Scan(
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
        component.sxx +=
            SumSquares(run.x1) - (run.x0 > 0 ? SumSquares(run.x0 - 1) : 0);
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
        blob.circularity =
            static_cast<float>(elongation * std::min(1.0, blob.fill / (kPi * 0.25)));
        blob.angle = static_cast<float>(0.5 * std::atan2(2.0 * xy, xx - yy));
        blob.length = static_cast<float>(std::sqrt(12.0 * major) * sample_step);
        blob.thickness = static_cast<float>(std::sqrt(12.0 * minor) * sample_step);
        blob.clipped = c.x0 == 0 || c.y0 == 0
                    || c.x1 + 1 >= (window.width + sample_step - 1) / sample_step
                    || c.y1 + 1 >= (window.height + sample_step - 1) / sample_step;
    }
    return std::span<const Blob>(candidates_.get(), count);
}

inline std::uint32_t BlobWorkspace::Root(std::uint32_t id) noexcept {
    while (runs_[id].parent != id) {
        runs_[id].parent = runs_[runs_[id].parent].parent;
        id = runs_[id].parent;
    }
    return id;
}

inline void BlobWorkspace::Unite(std::uint32_t a, std::uint32_t b) noexcept {
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

} // namespace dart::detection

#endif // DART_SRC_DETECTOR_BLOB_WORKSPACE_CPP
