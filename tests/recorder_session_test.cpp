#ifndef DART_TESTS_RECORDER_SESSION_TEST_CPP
# define DART_TESTS_RECORDER_SESSION_TEST_CPP

# include <array>
# include <cassert>
# include <cerrno>
# include <cstdio>
# include <cstdlib>
# include <filesystem>
# include <fstream>
# include <string>
# include <thread>
# include <unistd.h>

# include "camera/frame_lease.cpp"
# include "runtime/recordor/recorder.cpp"

namespace dart::tests {

class RecorderSessionTest final {
public:
    static void run() {
        char path[] = "/tmp/dart-recorder-session-XXXXXX";
        assert(::mkdtemp(path) != nullptr);
        const std::filesystem::path root(path);
        dart::recorder::Config config{};
        config.width = 32;
        config.height = 2;
        config.output_directory = path;
        config.keep_sessions = 2;

        for (int i = 1; i <= 3; ++i) {
            dart::recorder::Recorder recorder(config);
            assert(recorder.start());
            recorder.stop();
            const auto session = root / ("rec_000" + std::to_string(i));
            assert(std::filesystem::is_directory(session / "img"));
            assert(std::filesystem::exists(session / "logs/frames.csv"));
        }
        assert(!std::filesystem::exists(root / "rec_0001"));
        assert(std::filesystem::exists(root / "rec_0002"));
        assert(std::filesystem::exists(root / "rec_0003"));

        std::ofstream(root / "rec_0002/img/user.txt") << "keep";
        dart::recorder::Recorder guarded(config);
        assert(!guarded.start());
        assert(guarded.last_system_error() == EPERM);
        assert(std::filesystem::exists(root / "rec_0002/img/user.txt"));
        assert(std::filesystem::exists(root / "rec_0003/logs/frames.csv"));
        assert(!std::filesystem::exists(root / "rec_0004"));

        std::filesystem::remove_all(root);
        images(dart::recorder::ImageFormat::kPgm);
        images(dart::recorder::ImageFormat::kPbm);
        std::puts("recorder session retention passed");
    }

private:
    static std::expected<void, dart::camera::CameraError>
        release(dart::camera::FrameLeaseAccess::Slot* slot) noexcept {
        slot->state = dart::camera::FrameLeaseAccess::SlotState::kFree;
        return {};
    }

    static void images(dart::recorder::ImageFormat format) {
        char path[] = "/tmp/dart-recorder-images-XXXXXX";
        assert(::mkdtemp(path) != nullptr);
        std::array<std::byte, 64> pixels{};
        pixels.fill(std::byte{255});
        dart::camera::FrameLeaseAccess::Slot slot{};
        slot.view.luma = pixels;
        slot.view.width = slot.view.stride = 32;
        slot.view.height = 2;
        slot.view.sequence = 17;
        slot.view.received = std::chrono::microseconds{1'000'000};
        slot.state = dart::camera::FrameLeaseAccess::SlotState::kHeld;
        slot.release = &RecorderSessionTest::release;
        dart::recorder::Config config{};
        config.width = 32;
        config.height = 2;
        config.image_format = format;
        config.output_directory = path;
        dart::recorder::Recorder recorder{config};
        assert(recorder.start());
        auto lease = dart::camera::FrameLeaseAccess::make(&slot);
        bool accepted = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto admission = recorder.try_submit(std::move(lease));
            if (admission == dart::recorder::Admission::kAccepted) {
                accepted = true;
                break;
            }
            // Admission may race the worker's queue lock. Keep ownership and
            // retry in a later sampling interval, as a capture loop would.
            assert(admission == dart::recorder::Admission::kBusy && lease.valid());
            slot.view.received += std::chrono::milliseconds{50};
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        assert(accepted);
        assert(!lease.valid());
        recorder.stop();
        assert(recorder.pending_releases() == 1);
        assert(recorder.drain_releases() == 1);
        assert(slot.state == dart::camera::FrameLeaseAccess::SlotState::kFree);
        assert(recorder.stats().written == 1);
        assert(!recorder.start());
        assert(recorder.last_error() == dart::recorder::Error::kAlreadyStarted);
        const bool pbm = format == dart::recorder::ImageFormat::kPbm;
        const auto root = std::filesystem::path{path} / "rec_0001";
        const auto image = root / (pbm ? "img/frame_00000001.pbm" : "img/frame_00000001.pgm");
        std::ifstream input(image, std::ios::binary);
        const std::string actual{std::istreambuf_iterator<char>{input}, {}};
        const std::string header = pbm ? "P4\n32 2\n" : "P5\n32 2\n255\n";
        assert(actual.substr(0, header.size()) == header);
        assert(actual.size() == header.size() + (pbm ? 8 : 64));
        for (std::size_t i = header.size(); i < actual.size(); ++i)
            assert(static_cast<unsigned char>(actual[i]) == (pbm ? 0 : 255));
        std::ifstream csv(root / "logs/frames.csv");
        const std::string events{std::istreambuf_iterator<char>{csv}, {}};
        assert(events.find(",17,") != std::string::npos);
        assert(events.find(",accepted,") != std::string::npos);
        std::filesystem::remove_all(path);
    }
};

} // namespace dart::tests

#endif // DART_TESTS_RECORDER_SESSION_TEST_CPP

int main() { dart::tests::RecorderSessionTest::run(); }
