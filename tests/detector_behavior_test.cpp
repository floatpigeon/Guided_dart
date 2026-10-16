

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>

#include "src/detector/roi.cpp"

namespace dart::tests {

class DetectorBehaviorTest final {
public:
    static void run() {
        std::array<std::byte, 64 * 64> pixels{};
        for (std::size_t y = 28; y < 36; ++y)
            for (std::size_t x = 28; x < 36; ++x)
                pixels[y * 64 + x] = std::byte{255};
        camera::FrameView frame{};
        frame.luma = pixels;
        frame.width = frame.height = frame.stride = 64;
        frame.sequence = 1;
        frame.received = std::chrono::microseconds{1000};

        detection::BlobWorkspace workspace;
        auto unprepared = workspace.Scan(frame, {0, 0, 64, 64}, 128);
        assert(!unprepared && unprepared.error() == detection::DetectionError::kNotPrepared);
        assert(workspace.Prepare(64, 64));
        auto blobs = workspace.Scan(frame, {0, 0, 64, 64}, 128);
        assert(blobs && blobs->size() == 1);
        assert((*blobs)[0].area == 64);
        assert(std::abs((*blobs)[0].center.x - 31.5F) < 0.001F);
        detection::LightDetector light;
        assert(light.Detect(*blobs, 4096).valid);
        assert(!workspace.Scan(frame, {63, 0, 2, 64}, 128));
        detection::BlobWorkspace::Config small{};
        small.max_runs = small.max_candidates = 1;
        assert(workspace.Prepare(64, 64, small));
        auto exhausted = workspace.Scan(frame, {0, 0, 64, 64}, 128);
        assert(!exhausted && exhausted.error() == detection::DetectionError::kWorkspaceExhausted);

        detection::RoiDetector::Config config{};
        config.light.search_step = 1;
        config.light.confirm_frames = config.light.far_confirm_frames = 1;
        detection::RoiDetector detector{config};
        assert(detector.Prepare(64, 64));
        auto measured = detector.Detect(frame);
        assert(measured && measured->light.valid);
        assert(measured->source.frame_id == 1);
        auto duplicate = detector.Detect(frame);
        assert(!duplicate && duplicate.error() == detection::DetectionError::kOutOfOrderFrame);
        pixels.fill(std::byte{0});
        frame.sequence = 2;
        frame.received += std::chrono::milliseconds{10};
        auto missing = detector.Detect(frame);
        assert(missing && !missing->target.valid && missing->kind == detection::TargetKind::kNone);
        detector.Reset();
        frame.sequence = 1;
        assert(detector.Detect(frame));

        std::array<detection::Blob, 2> bars{};
        for (std::size_t i = 0; i < bars.size(); ++i) {
            auto& bar = bars[i];
            bar.bounds = {static_cast<std::uint32_t>(20 + i * 20), 20, 2, 12};
            bar.center = {static_cast<float>(bar.bounds.x) + 0.5F, 25.5F};
            bar.area = 24;
            bar.fill = 1.0F;
            bar.length = 12.0F;
            bar.thickness = 2.0F;
            bar.angle = 1.57079632679F;
        }
        detection::Target anchor{};
        anchor.valid = true;
        anchor.center = {30.5F, 50.0F};
        anchor.bounds = {27, 46, 8, 8};
        anchor.radius = 4.0F;
        detection::ArmorDetector armor;
        const auto plate = armor.Detect(bars, anchor);
        assert(plate.valid);
        assert(std::abs(plate.center.x - 30.5F) < 0.001F);
        assert(std::abs(plate.center.y - 25.5F) < 0.001F);
        anchor.valid = false;
        assert(!armor.Detect(bars, anchor).valid);
    }
};

} // namespace dart::tests

int main() { dart::tests::DetectorBehaviorTest::run(); }
