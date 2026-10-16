#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/imu/session.cpp"

namespace dart::tests {

class ImuSessionTest final {
    class Source final : public imu::IImu {
    public:
        std::expected<imu::Sample, imu::ErrorCode> read_sample() noexcept override {
            std::lock_guard lock(mutex_);
            auto result = pending_;
            pending_ = std::unexpected(imu::ErrorCode::kBusTransfer);
            return result;
        }
        void push(const imu::Sample& sample) noexcept {
            std::lock_guard lock(mutex_);
            pending_ = sample;
        }
    private:
        std::mutex mutex_;
        std::expected<imu::Sample, imu::ErrorCode> pending_{std::unexpected(imu::ErrorCode::kBusTransfer)};
    };

    struct Expected {
        std::string kind;
        imu::Estimator::Estimate estimate;
        bool command_accepted = false;
        std::uint32_t command_quality = 0;
        imu::Quaternionf reference{};
    };

    static imu::Estimator::Estimate feed(imu::Session& session, Source& source, const imu::Sample& sample) {
        const auto count = session.stats().samples;
        source.push(sample);
        for (int i = 0; i < 500 && session.stats().samples == count; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        assert(session.stats().samples == count + 1);
        return session.current();
    }

    static std::vector<std::string> split(const std::string& line) {
        std::vector<std::string> fields;
        std::size_t begin = 0;
        for (;;) {
            const auto end = line.find(',', begin);
            fields.push_back(line.substr(begin, end - begin));
            if (end == std::string::npos)
                return fields;
            begin = end + 1;
        }
    }

    static void verify(const std::map<std::string, std::string>& row, const Expected& expected) {
        const auto& e = expected.estimate;
        const auto& s = e.sample;
        const auto integer = [&](const char* name, std::uint64_t value) {
            assert(std::stoull(row.at(name)) == value);
        };
        const auto scalar = [&](const std::string& name, float value) {
            const float logged = std::strtof(row.at(name).c_str(), nullptr);
            assert(logged == value || (std::isnan(logged) && std::isnan(value)));
        };
        const auto vector = [&](const char* x, const char* y, const char* z, imu::Vector3f value) {
            scalar(x, value.x);
            scalar(y, value.y);
            scalar(z, value.z);
        };
        assert(row.at("event") == expected.kind);
        integer("timestamp_ns", s.timestamp_ns);
        integer("generation", e.generation);
        integer("quality_events", e.quality_events);
        integer("sample_accepted", e.sample_accepted);
        integer("integrated", e.integrated);
        integer("has_reference_frame", e.has_reference_frame);
        integer("command_accepted", expected.command_accepted);
        integer("command_quality_events", expected.command_quality);
        integer("sample_flags", s.flags);
        assert(std::stoi(row.at("raw_ax")) == s.raw_counts.accelerometer.x);
        assert(std::stoi(row.at("raw_ay")) == s.raw_counts.accelerometer.y);
        assert(std::stoi(row.at("raw_az")) == s.raw_counts.accelerometer.z);
        assert(std::stoi(row.at("raw_gx")) == s.raw_counts.gyroscope.x);
        assert(std::stoi(row.at("raw_gy")) == s.raw_counts.gyroscope.y);
        assert(std::stoi(row.at("raw_gz")) == s.raw_counts.gyroscope.z);
        vector("ax_m_s2", "ay_m_s2", "az_m_s2", s.acceleration_m_s2);
        vector("gx_rad_s", "gy_rad_s", "gz_rad_s", s.angular_velocity_rad_s);
        scalar("temperature_c", s.temperature_c);
        scalar("qw", e.attitude.w);
        scalar("qx", e.attitude.x);
        scalar("qy", e.attitude.y);
        scalar("qz", e.attitude.z);
        vector("roll_rad", "pitch_rad", "yaw_rad", e.attitude_rpy_rad);
        vector("wx_rad_s", "wy_rad_s", "wz_rad_s", e.angular_velocity_rad_s);
        vector("linear_ax_m_s2", "linear_ay_m_s2", "linear_az_m_s2", e.linear_acceleration_m_s2);
        vector("vx_m_s", "vy_m_s", "vz_m_s", e.velocity_m_s);
        vector("rotation_x_rad", "rotation_y_rad", "rotation_z_rad", e.cumulative_rotation_rad);
        scalar("seed_qw", expected.reference.w);
        scalar("seed_qx", expected.reference.x);
        scalar("seed_qy", expected.reference.y);
        scalar("seed_qz", expected.reference.z);
    }

public:
    static void run() {
        char directory[] = "/tmp/dart-imu-session-XXXXXX";
        assert(::mkdtemp(directory) != nullptr);
        const auto path = std::filesystem::path{directory} / "imu.log";
        Source source;
        imu::Session session{source};
        assert(!session.current().has_reference_frame);
        assert(!session.start(nullptr));
        assert(!session.start(path.c_str(), 0));
        assert(!session.start(directory));
        assert(session.start(path.c_str()));
        for (int i = 0; i < 500 && session.stats().read_errors == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        assert(session.stats().read_errors > 0);
        std::vector<Expected> expected;
        imu::Sample sample{};
        sample.timestamp_ns = 1'000'000'000;
        sample.acceleration_m_s2.z = 9.80665F;
        sample.raw_counts.accelerometer = {-123, 456, 2048};
        sample.raw_counts.gyroscope = {321, -654, 987};
        sample.temperature_c = 26.125F;
        auto state = feed(session, source, sample);
        assert(state.sample_accepted && !state.integrated && !state.has_reference_frame);
        expected.push_back({"sample", state});
        auto seeded = session.seed({});
        assert(seeded.accepted && seeded.generation == 1);
        expected.push_back({"seed", session.current(), true});

        sample.timestamp_ns += 10'000'000;
        sample.acceleration_m_s2.x = 2;
        sample.angular_velocity_rad_s.z = 1;
        state = feed(session, source, sample);
        assert(state.integrated && state.velocity_m_s.x > 0 && state.cumulative_rotation_rad.z > 0);
        expected.push_back({"sample", state});
        seeded = session.seed({}); // Moving samples cannot start a new integral.
        assert(!seeded.accepted && seeded.generation == 1);
        expected.push_back({"seed", session.current(), false, seeded.quality_events});

        state = feed(session, source, sample); // Duplicate timestamp, preserve rejection flags.
        assert(!state.sample_accepted);
        expected.push_back({"sample", state});
        sample.timestamp_ns += 10'000'000;
        sample.flags = static_cast<std::uint32_t>(imu::SampleFlag::kAccelerometerSaturated);
        state = feed(session, source, sample);
        assert(!state.sample_accepted);
        expected.push_back({"sample", state});
        sample.flags = 0;
        sample.timestamp_ns += 10'000'000;
        sample.acceleration_m_s2.x = std::numeric_limits<float>::quiet_NaN();
        state = feed(session, source, sample);
        assert(!state.sample_accepted);
        expected.push_back({"sample", state});
        sample.acceleration_m_s2.x = 0;
        sample.angular_velocity_rad_s.z = 0;
        sample.timestamp_ns += 500'000'000;
        state = feed(session, source, sample);
        assert(state.sample_accepted && !state.integrated && state.quality_events != 0);
        expected.push_back({"sample", state});

        const imu::Quaternionf reference{0.70710677F, 0, 0, 0.70710677F};
        seeded = session.seed(reference);
        state = session.current();
        assert(seeded.accepted && state.generation == 2);
        assert(state.velocity_m_s.x == 0 && state.cumulative_rotation_rad.z == 0);
        expected.push_back({"seed", state, true, 0, reference});
        const imu::Quaternionf invalid{0, 0, 0, 0};
        seeded = session.seed(invalid);
        assert(!seeded.accepted && seeded.generation == 2);
        expected.push_back({"seed", session.current(), false, seeded.quality_events, invalid});
        sample.timestamp_ns += 10'000'000;
        sample.acceleration_m_s2.x = 2;
        state = feed(session, source, sample);
        assert(state.integrated && state.velocity_m_s.y > 0);
        expected.push_back({"sample", state});
        session.reset();
        state = session.current();
        assert(state.generation == 3 && !state.has_reference_frame);
        expected.push_back({"reset", state, true});
        sample.timestamp_ns += 10'000'000;
        state = feed(session, source, sample);
        assert(!state.integrated && !state.has_reference_frame);
        expected.push_back({"sample", state});
        session.stop();
        session.stop();
        assert(!session.start(path.c_str()));
        const auto stats = session.stats();
        assert(stats.dropped == 0 && stats.io_errors == 0);
        assert(stats.read_errors > 0);
        assert(stats.written == expected.size() + stats.read_errors);

        std::ifstream input{path};
        std::string line;
        assert(std::getline(input, line) && line.find("# imu_log version=1") == 0);
        assert(std::getline(input, line));
        const auto header = split(line);
        std::size_t next = 0;
        std::uint64_t previous_sequence = 0;
        bool footer = false;
        while (std::getline(input, line)) {
            if (line.starts_with("# session_end")) {
                footer = true;
                continue;
            }
            const auto fields = split(line);
            assert(fields.size() == header.size());
            std::map<std::string, std::string> row;
            for (std::size_t i = 0; i < fields.size(); ++i)
                row.emplace(header[i], fields[i]);
            assert(std::stoull(row.at("sequence")) == ++previous_sequence);
            assert(std::stoull(row.at("recorded_ns")) > 0);
            if (row.at("event") == "read_error") {
                assert(std::stoi(row.at("read_error")) == static_cast<int>(imu::ErrorCode::kBusTransfer));
                continue;
            }
            assert(next < expected.size());
            verify(row, expected[next++]);
        }
        assert(footer && next == expected.size());

        // Startup seed is a one-time request. Reset must wait for a new command.
        Source initial_source;
        sample = {};
        sample.acceleration_m_s2.z = 9.80665F;
        sample.timestamp_ns = 2'000'000'000;
        initial_source.push(sample);
        imu::Session initial{initial_source};
        assert(initial.start(path.c_str(), 1000, true));
        for (int i = 0; i < 500 && !initial.current().has_reference_frame; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        assert(initial.current().generation == 1);
        initial.reset();
        sample.timestamp_ns += 10'000'000;
        assert(!feed(initial, initial_source, sample).has_reference_frame);
        initial.stop();

        imu::Session full{source};
        assert(!full.start("/dev/full"));
        assert(full.stats().io_errors > 0);
        std::filesystem::remove_all(directory);
    }
};

} // namespace dart::tests

int main() { dart::tests::ImuSessionTest::run(); }
