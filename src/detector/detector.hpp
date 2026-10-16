#ifndef DART_SRC_DETECTOR_DETECTOR_HPP_
#define DART_SRC_DETECTOR_DETECTOR_HPP_

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include "camera/camera.hpp"

// Vision consumers include this header and link detector.cpp and roi.cpp.
namespace dart::detection {

// Geometry uses full-image pixel coordinates. Right/bottom edges are exclusive.
struct Point {
    float x = 0.0F;
    float y = 0.0F;
};

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

enum class TargetKind : std::uint8_t { kNone, kLight, kArmor };

// A measured target; an invalid target never represents a held old measurement.
struct Target {
    bool valid = false;
    Point center{};
    Rect bounds{};
    std::uint32_t area = 0;
    float radius = 0.0F;
    float quality = 0.0F;
    bool clipped = false;
};

// Connected-component measurements shared by the light and armor detectors.
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

struct WorkspaceConfig {
    std::uint32_t max_runs = 16384;
    std::uint32_t max_candidates = kMaxCandidates;
};

// Defaults are starting points, UNVALIDATED on v2 hardware. Y8 has no color.
struct LightConfig {
    std::uint32_t min_area = 3;
    std::uint32_t far_area_threshold = 30;
    std::uint32_t far_size_px = 8;
    // Acquisition uses sampling; locked ROI tracking scans at step 1.
    std::uint32_t search_step = 2;
    std::uint32_t confirm_frames = 2;
    std::uint32_t far_confirm_frames = 3;
    float far_jump_k = 1.5F;
    std::uint32_t far_jump_min_px = 6;
    std::uint32_t deadband_px = 2;
    float min_fill = 0.5F;
    float min_circularity = 0.0F;
    float circularity_weight = 1.0F;
    float aspect_min = 0.7F;
    float aspect_max = 1.4F;
    float max_area_fraction = 0.25F;
};

struct ArmorConfig {
    std::uint32_t length_floor = 2;
    std::uint32_t area_floor = 2;
    float length_min_k = 0.2F;
    float length_max_k = 8.0F;
    float tiny_px = 8.0F;
    float aspect_min = 2.0F;
    float fill_min = 0.5F;
    float flat_deg = 12.0F;
    float parallel_deg = 15.0F;
    float thickness_ratio = 0.3F;
    float length_ratio = 0.4F;
    float overlap = 0.4F;
    float gap_max_k = 10.0F;
    float diagonal_min_sin = 0.30F;
    float border_penalty = 0.0F;
    float window_width_k = 3.0F;
    float window_height_k = 2.0F;
    float window_gap_k = 0.0F;
    float window_min_side = 24.0F;
    float near_k = 8.0F;
    float near_min_px = 32.0F;
    float span_min_k = 0.2F;
    float span_max_k = 12.0F;
    std::uint32_t plate_up_lo_pct = 0;
    std::uint32_t plate_up_hi_pct = 1200;
    std::uint32_t plate_offx_pct = 600;
    float association_k = 0.6F;
    float association_min_px = 4.0F;
    std::uint32_t track_hold = 2;
};

struct RoiConfig {
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

struct DetectionConfig {
    std::uint8_t threshold = 50;
    WorkspaceConfig workspace{};
    LightConfig light{};
    ArmorConfig armor{};
    RoiConfig roi{};
};

// Source identity is copied from FrameView; no pixel or lease pointers escape.
struct FrameMetadata {
    std::uint64_t frame_id = 0;
    std::uint64_t source_pts = 0;
    std::chrono::microseconds received{};
    bool source_pts_timebase_validated = false;
};

struct ScanMetadata {
    Rect search_window{};
    bool full_frame = false;
    bool rescanned = false;
    std::uint32_t candidates = 0;
};

struct DetectResult {
    TargetKind kind = TargetKind::kNone;
    Target target{}; // Armor center when available, otherwise light center.
    Target light{};
    Target armor{};
    FrameMetadata source{};
    ScanMetadata scan{};
    std::uint32_t missed_frames = 0;
};

// Consumers need only detection and reset, independent of workspace setup.
class IDetector {
public:
    virtual ~IDetector() = default;
    virtual std::expected<DetectResult, DetectionError>
        Detect(const camera::FrameView& frame) noexcept = 0;
    virtual void Reset() noexcept = 0;
};

// Allocates only in Prepare(). Scan() borrows pixels synchronously; its
// returned candidates are valid until the next Scan()/Prepare(). Pixels are
// read-only.
class BlobWorkspace {
public:
    BlobWorkspace() noexcept;
    ~BlobWorkspace();
    BlobWorkspace(const BlobWorkspace&) = delete;
    BlobWorkspace& operator=(const BlobWorkspace&) = delete;

    std::expected<void, DetectionError> Prepare(
        std::uint32_t max_width, std::uint32_t max_height, WorkspaceConfig config = {}) noexcept;
    std::expected<void, DetectionError> Validate(const camera::FrameView& frame) const noexcept;
    std::expected<std::span<const Blob>, DetectionError> Scan(
        const camera::FrameView& frame, Rect window, std::uint8_t threshold,
        std::uint32_t min_area = 2, std::uint32_t sample_step = 1) noexcept;

    std::uint32_t runs() const noexcept { return run_count_; }
    std::uint32_t run_high_water() const noexcept { return run_high_water_; }

private:
    struct Run;
    struct Component;

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

// Stateless per-frame selection; temporal confirmation belongs to LightTracker.
class LightDetector {
public:
    explicit LightDetector(LightConfig config = {}) noexcept;
    bool ValidConfig() const noexcept;
    Target Detect(std::span<const Blob> blobs, std::uint64_t frame_area) const noexcept;

private:
    LightConfig config_;
};

// Pairs measured light bars using the current frame's light as an anchor.
class ArmorDetector {
public:
    explicit ArmorDetector(ArmorConfig config = {}) noexcept;
    bool ValidConfig() const noexcept;
    Target Detect(
        std::span<const Blob> blobs, const Target& light,
        const Target* previous = nullptr) const noexcept;
    Rect
        SearchWindow(const Target& light, std::uint32_t width, std::uint32_t height) const noexcept;

private:
    ArmorConfig config_;
};

// Implementation details used by RoiDetector; vision consumers use IDetector.
namespace internal {

class LightTracker {
public:
    explicit LightTracker(LightConfig config) noexcept;
    bool ValidConfig() const noexcept;
    bool Confirm(Target& light, std::uint64_t frame_id, const Target& previous) noexcept;
    void ClearConfirmation() noexcept;
    void Reset() noexcept;
    bool has_measurement() const noexcept { return state_.measurement_valid; }

private:
    struct State {
        Target last_measurement{};
        std::uint32_t hits = 0;
        std::uint64_t last_frame_id = 0;
        bool seen_frame = false;
        bool input_valid = false;
        bool measurement_valid = false;
        bool locked = false;
    };

    LightConfig config_;
    State state_{};
};

struct RoiState {
    Rect envelope{};
    std::uint32_t missed_frames = 0;
    std::uint32_t frames_since_full = 0;
};

// Window geometry and validation; it does not run detection or mutate tracking.
class RoiPolicy {
public:
    explicit RoiPolicy(RoiConfig config) noexcept;
    bool ValidConfig() const noexcept;
    Rect NextWindow(
        std::uint32_t width, std::uint32_t height, bool has_light,
        const RoiState& state) const noexcept;

private:
    RoiConfig config_;
};

} // namespace internal

// One instance per ordered capture pipeline. The caller owns and releases frame
// leases. Prepare() allocates at startup; Detect() never retains frame pixels.
class RoiDetector final : public IDetector {
public:
    explicit RoiDetector(DetectionConfig config = {}) noexcept;
    RoiDetector(const RoiDetector&) = delete;
    RoiDetector& operator=(const RoiDetector&) = delete;

    std::expected<void, DetectionError>
        Prepare(std::uint32_t max_width, std::uint32_t max_height) noexcept;
    std::expected<DetectResult, DetectionError>
        Detect(const camera::FrameView& frame) noexcept override;
    void Reset() noexcept override;
    std::uint32_t run_high_water() const noexcept { return workspace_.run_high_water(); }

private:
    struct TargetHistory {
        Target light{};
        Target armor{};
        std::uint32_t armor_misses = 0;
    };

    struct FrameHistory {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint64_t sequence = 0;
        std::chrono::microseconds received{};
        bool valid = false;
    };

    std::expected<void, DetectionError>
        Measure(const camera::FrameView& frame, Rect window, DetectResult& result) noexcept;
    void UpdateTracking(DetectResult& result, std::uint32_t width, std::uint32_t height) noexcept;
    void ResetTracking() noexcept;

    DetectionConfig config_;
    LightDetector light_;
    ArmorDetector armor_;
    BlobWorkspace workspace_;
    internal::LightTracker light_tracker_;
    internal::RoiPolicy roi_policy_;
    TargetHistory targets_{};
    FrameHistory frame_{};
    internal::RoiState roi_state_{};
};

} // namespace dart::detection

#endif // DART_SRC_DETECTOR_DETECTOR_HPP_
