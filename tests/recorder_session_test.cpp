#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "runtime/recordor/recorder.cpp"

int main()
{
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
    std::puts("recorder session retention passed");
}
