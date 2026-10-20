#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "src/startup/services.cpp"
#include "camera/c_api/raw10.h"

namespace dart::tests {

class BinaryPipelineTest final {
    static constexpr std::uint32_t kWidth = 320, kHeight = 240;
    inline static std::uint32_t threshold_;
    inline static camera::Raw10Layout layout_;

    class Camera final : public camera::ICamera {
    public:
        explicit Camera(const camera::Controls& controls) {
            assert(controls.binary_threshold == threshold_ && controls.raw10_layout == layout_);
            std::vector<std::uint8_t> raw(kWidth * kHeight * 2);
            for (std::uint32_t y = 0; y < kHeight; ++y) {
                for (std::uint32_t x = 0; x < kWidth; ++x) {
                    std::uint32_t sample = foreground(x, y) ? 200 : 199;
                    if (layout_ == camera::Raw10Layout::kLe16Msb)
                        sample <<= 6;
                    const auto offset = (y * kWidth + x) * 2;
                    raw[offset] = static_cast<std::uint8_t>(sample);
                    raw[offset + 1] = static_cast<std::uint8_t>(sample >> 8);
                }
            }
            assert(
                dart_camera_raw10_binary(
                    raw.data(), raw.size(), kWidth, kHeight, kWidth * 2,
                    static_cast<std::uint32_t>(layout_), threshold_, 0,
                    reinterpret_cast<std::uint8_t*>(pixels_.data()), pixels_.size())
                == 0);
            for (auto& slot : slots_) {
                slot.view.luma = pixels_;
                slot.view.width = slot.view.stride = kWidth;
                slot.view.height = kHeight;
                slot.release = release;
            }
        }
        std::expected<void, camera::CameraError> start() noexcept override {
            state_ = camera::State::kStarted;
            return {};
        }
        std::expected<camera::AcquireResult, camera::CameraError>
            acquire(std::chrono::milliseconds) noexcept override {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            camera::AcquireResult result{};
            for (auto& slot : slots_) {
                if (slot.state != camera::FrameLeaseAccess::SlotState::kFree)
                    continue;
                slot.state = camera::FrameLeaseAccess::SlotState::kHeld;
                slot.view.sequence = ++sequence_;
                slot.view.received = std::chrono::microseconds{sequence_ * 20'000};
                result.status = camera::AcquireStatus::kFrame;
                result.lease = camera::FrameLeaseAccess::make(&slot);
                return result;
            }
            result.status = camera::AcquireStatus::kLeaseExhausted;
            return result;
        }
        std::expected<void, camera::CameraError> stop() noexcept override {
            assert(outstanding() == 0);
            state_ = camera::State::kInitialized;
            return {};
        }
        camera::State state() const noexcept override { return state_; }
        const camera::FrameInfo& frame_info() const noexcept override { return info_; }
        std::expected<void, camera::CameraError>
            set_fixed_exposure_lines(std::uint32_t) noexcept override {
            return {};
        }
        std::uint32_t capacity() const noexcept override { return slots_.size(); }
        std::uint32_t outstanding() const noexcept override {
            std::uint32_t count = 0;
            for (const auto& slot : slots_)
                count += slot.state != camera::FrameLeaseAccess::SlotState::kFree;
            return count;
        }

    private:
        static std::expected<void, camera::CameraError>
            release(camera::FrameLeaseAccess::Slot* slot) noexcept {
            slot->state = camera::FrameLeaseAccess::SlotState::kFree;
            return {};
        }
        camera::State state_ = camera::State::kInitialized;
        camera::FrameInfo info_{kWidth, kHeight};
        std::uint64_t sequence_ = 0;
        std::array<std::byte, kWidth * kHeight> pixels_{};
        std::array<camera::FrameLeaseAccess::Slot, 4> slots_{};
    };

    static bool foreground(std::uint32_t x, std::uint32_t y) {
        return x >= 100 && x < 108 && y >= 100 && y < 108;
    }
    static std::expected<std::unique_ptr<camera::ICamera>, camera::CameraError>
        create(const camera::Controls& controls) noexcept {
        return std::unique_ptr<camera::ICamera>{new Camera{controls}};
    }
    static std::string read(const std::filesystem::path& path) {
        std::ifstream input{path, std::ios::binary};
        assert(input);
        return {std::istreambuf_iterator<char>{input}, {}};
    }

public:
    static void run(bool pbm) {
        char directory[] = "/tmp/dart-binary-pipeline-XXXXXX";
        assert(::mkdtemp(directory));
        const auto root = std::filesystem::path{directory};
        app::Arguments args{};
        args.seconds = 1;
        args.record_format = pbm ? recorder::ImageFormat::kPbm : recorder::ImageFormat::kPgm;
        args.threshold = threshold_ = pbm ? 0 : 50;
        args.raw10_layout = layout_ =
            pbm ? camera::Raw10Layout::kLe16Lsb : camera::Raw10Layout::kLe16Msb;
        assert(app::copy_path(args.log_path, sizeof(args.log_path), (root / "app.log").string()));
        assert(app::copy_path(args.rec_dir, sizeof(args.rec_dir), (root / "records").string()));
        platform::RunControl control{args.seconds};
        logging::Logger log_sink;
        assert(log_sink.open(args.log_path));
        assert(app::run_services(args, create, control, log_sink) == 0);
        const auto log = read(root / "app.log");
        assert(log.find("op=detector_prepare result=ok") != std::string::npos);
        assert(log.find("level=ERROR") == std::string::npos);
        const auto image =
            read(root / "records/rec_0001/img" / (pbm ? "recording.pbm" : "recording.pgm"));
        const std::string header = pbm ? "P4\n320 240\n" : "P5\n320 240\n255\n";
        const std::size_t payload = pbm ? kWidth * kHeight / 8 : kWidth * kHeight;
        const auto record = header.size() + payload;
        assert(image.size() >= record && image.size() % record == 0);
        for (std::size_t offset = 0; offset < image.size(); offset += record) {
            assert(image.substr(offset, header.size()) == header);
            for (std::size_t i = 0; i < payload; ++i) {
                const auto expected = pbm ? 0 : foreground(i % kWidth, i / kWidth) ? 255 : 0;
                assert(static_cast<unsigned char>(image[offset + header.size() + i]) == expected);
            }
        }
        std::filesystem::remove_all(root);
    }
};

} // namespace dart::tests

int main() {
    dart::tests::BinaryPipelineTest::run(true);
    dart::tests::BinaryPipelineTest::run(false);
}
