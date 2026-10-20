#ifndef DART_RUNTIME_IMU_SESSION_CPP
#define DART_RUNTIME_IMU_SESSION_CPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include "imu/estimator.cpp"
#include "imu/interface.cpp"

namespace dart::imu {

/* Owns the estimator shared by consumers and the log. Device lifetime encloses
 * Session lifetime. start()/stop() belong to the application lifecycle thread;
 * current()/seed()/reset() may be called by consumers while sampling runs. */
class Session final {
public:
    struct Stats {
        std::uint64_t samples = 0;
        std::uint64_t read_errors = 0;
        std::uint64_t written = 0;
        std::uint64_t dropped = 0;
        std::uint64_t io_errors = 0;
        std::uint64_t log_bytes = 0;
        bool log_limit_reached = false;
    };

    static constexpr std::uint64_t kDefaultLogLimitBytes = 32ULL * 1024 * 1024;

    explicit Session(IImu& source, const Estimator::Config& config = {}) noexcept
        : source_(source)
        , config_(config)
        , estimator_(config) {}
    ~Session() { stop(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /* One run per instance. The application may request an initial stationary
     * seed; reset() never silently seeds again. Paths must already have parents. */
    bool start(
        const char* path, std::uint32_t period_us = 1000, bool initial_seed = false,
        std::uint64_t log_limit_bytes = kDefaultLogLimitBytes) noexcept;
    void stop() noexcept;
    Estimator::Estimate current() const noexcept;
    Estimator::SeedResult seed(const Quaternionf& sensor_to_reference) noexcept;
    void reset() noexcept;
    Stats stats() const noexcept;

private:
    enum class EventKind { kSample, kSeed, kReset, kReadError };

    struct Event {
        EventKind kind = EventKind::kSample;
        Estimator::Estimate estimate{};
        Quaternionf requested_reference{};
        std::uint64_t sequence = 0;
        std::uint64_t recorded_ns = 0;
        std::uint64_t dropped = 0;
        std::uint32_t command_quality_events = 0;
        int read_error = -1;
        bool command_accepted = false;
    };

    static constexpr std::size_t kQueueCapacity = 512;
    static std::uint64_t now_ns() noexcept {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    void enqueue(Event event) noexcept; // Called with state_mutex_; never waits for disk.
    Estimator::SeedResult seed_locked(const Quaternionf& reference) noexcept;
    void sample_loop() noexcept;
    void writer_loop() noexcept;
    bool write_event(const Event& event) noexcept;
    bool write_log(const char* data, int length, std::size_t capacity) noexcept;
    static void* sample_entry(void* context) noexcept {
        static_cast<Session*>(context)->sample_loop();
        return nullptr;
    }
    static void* writer_entry(void* context) noexcept {
        static_cast<Session*>(context)->writer_loop();
        return nullptr;
    }

    IImu& source_;
    Estimator::Config config_;
    Estimator estimator_;
    mutable std::mutex state_mutex_;
    Estimator::Estimate latest_{estimator_.current()};
    std::uint64_t sequence_ = 0;
    bool initial_seed_pending_ = false;
    std::mutex queue_mutex_;
    std::condition_variable queue_ready_;
    std::unique_ptr<Event[]> queue_{};
    std::size_t head_ = 0, tail_ = 0, count_ = 0;
    bool writer_stopping_ = false;
    bool accepting_ = false;            // Protected by state_mutex_.
    std::mutex sleep_mutex_;
    std::condition_variable sample_wake_;
    std::atomic<bool> sampling_{false};
    std::uint32_t period_us_ = 1000;
    pthread_t sample_thread_{}, writer_thread_{};
    bool sample_started_ = false, writer_started_ = false, used_ = false;
    std::FILE* file_ = nullptr;
    std::atomic<std::uint64_t> samples_{0}, read_errors_{0}, written_{0}, dropped_{0},
        io_errors_{0};
    std::atomic<std::uint64_t> log_bytes_{0};
    std::atomic<bool> log_limit_reached_{false};
    std::uint64_t log_limit_bytes_ = kDefaultLogLimitBytes;
};

inline bool Session::start(
    const char* path, std::uint32_t period_us, bool initial_seed,
    std::uint64_t log_limit_bytes) noexcept {
    if (used_ || path == nullptr || path[0] == '\0' || period_us == 0 || log_limit_bytes == 0)
        return false;
    queue_.reset(new (std::nothrow) Event[kQueueCapacity]);
    if (!queue_)
        return false;
    file_ = std::fopen(path, "a");
    if (file_ == nullptr)
        return false;
    struct stat file_status{};
    if (::fstat(::fileno(file_), &file_status) != 0 || file_status.st_size < 0) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    log_bytes_.store(static_cast<std::uint64_t>(file_status.st_size));
    log_limit_bytes_ = log_limit_bytes;
    used_ = true;
    writer_stopping_ = false;
    period_us_ = period_us;
    initial_seed_pending_ = initial_seed;
    char header[2048];
    const int header_length = std::snprintf(
        header, sizeof(header),
        "# imu_log version=1 start_ns=%llu period_us=%u initial_seed=%d log_limit_bytes=%llu "
        "queue_capacity=%zu queue_bytes=%zu max_interval_ns=%llu "
        "max_acceleration_m_s2=%.9g max_rate_rad_s=%.9g "
        "max_acceleration_step_m_s2=%.9g max_rate_step_rad_s=%.9g "
        "seed_gravity_tolerance_m_s2=%.9g seed_max_rate_rad_s=%.9g\n"
        "event,sequence,recorded_ns,timestamp_ns,generation,quality_events,"
        "sample_accepted,integrated,has_reference_frame,command_accepted,command_quality_events,"
        "read_error,dropped,raw_ax,raw_ay,raw_az,raw_gx,raw_gy,raw_gz,"
        "ax_m_s2,ay_m_s2,az_m_s2,gx_rad_s,gy_rad_s,gz_rad_s,temperature_c,sample_flags,"
        "qw,qx,qy,qz,roll_rad,pitch_rad,yaw_rad,wx_rad_s,wy_rad_s,wz_rad_s,"
        "linear_ax_m_s2,linear_ay_m_s2,linear_az_m_s2,vx_m_s,vy_m_s,vz_m_s,"
        "rotation_x_rad,rotation_y_rad,rotation_z_rad,seed_qw,seed_qx,seed_qy,seed_qz\n",
        static_cast<unsigned long long>(now_ns()), period_us, initial_seed ? 1 : 0,
        static_cast<unsigned long long>(log_limit_bytes_), kQueueCapacity,
        sizeof(Event) * kQueueCapacity,
        static_cast<unsigned long long>(config_.max_integration_interval_ns),
        config_.max_acceleration_m_s2, config_.max_angular_velocity_rad_s,
        config_.max_acceleration_step_m_s2, config_.max_angular_velocity_step_rad_s,
        config_.seed_gravity_tolerance_m_s2, config_.seed_max_angular_velocity_rad_s);
    const bool header_ok = write_log(header, header_length, sizeof(header));
    if ((!header_ok && !log_limit_reached_.load()) || std::fflush(file_) != 0) {
        ++io_errors_;
        stop();
        return false;
    }
    if (::pthread_create(&writer_thread_, nullptr, &Session::writer_entry, this) != 0) {
        stop();
        return false;
    }
    writer_started_ = true;
    {
        std::lock_guard lock(state_mutex_);
        accepting_ = true;
    }
    sampling_.store(true);
    if (::pthread_create(&sample_thread_, nullptr, &Session::sample_entry, this) != 0) {
        stop();
        return false;
    }
    sample_started_ = true;
    return true;
}

inline void Session::stop() noexcept {
    {
        std::lock_guard lock(sleep_mutex_);
        sampling_.store(false);
    }
    sample_wake_.notify_all();
    if (sample_started_) {
        (void)::pthread_join(sample_thread_, nullptr);
        sample_started_ = false;
    }
    {
        std::lock_guard lock(state_mutex_);
        accepting_ = false;
    }
    {
        std::lock_guard lock(queue_mutex_);
        writer_stopping_ = true;
    }
    queue_ready_.notify_all();
    if (writer_started_) {
        (void)::pthread_join(writer_thread_, nullptr);
        writer_started_ = false;
    }
    if (file_ != nullptr) {
        char footer[256];
        const int footer_length = std::snprintf(
            footer, sizeof(footer),
            "# session_end samples=%llu read_errors=%llu written=%llu dropped=%llu "
            "io_errors=%llu\n",
            static_cast<unsigned long long>(samples_.load()),
            static_cast<unsigned long long>(read_errors_.load()),
            static_cast<unsigned long long>(written_.load()),
            static_cast<unsigned long long>(dropped_.load()),
            static_cast<unsigned long long>(io_errors_.load()));
        if (!write_log(footer, footer_length, sizeof(footer)) && !log_limit_reached_.load())
            ++io_errors_;
        if (std::fflush(file_) != 0)
            ++io_errors_;
        if (::fsync(::fileno(file_)) != 0)
            ++io_errors_;
        if (std::fclose(file_) != 0)
            ++io_errors_;
        file_ = nullptr;
    }
}

inline Estimator::Estimate Session::current() const noexcept {
    std::lock_guard lock(state_mutex_);
    return latest_;
}

inline Estimator::SeedResult Session::seed(const Quaternionf& reference) noexcept {
    std::lock_guard lock(state_mutex_);
    initial_seed_pending_ = false;
    return seed_locked(reference);
}

inline Estimator::SeedResult Session::seed_locked(const Quaternionf& reference) noexcept {
    const auto result = estimator_.seed(latest_.sample, reference);
    if (result.accepted)
        latest_ = estimator_.current();
    Event event{};
    event.kind = EventKind::kSeed;
    event.estimate = latest_;
    event.requested_reference = reference;
    event.command_accepted = result.accepted;
    event.command_quality_events = result.quality_events;
    enqueue(event);
    return result;
}

inline void Session::reset() noexcept {
    std::lock_guard lock(state_mutex_);
    initial_seed_pending_ = false;
    estimator_.reset();
    latest_ = estimator_.current();
    Event event{};
    event.kind = EventKind::kReset;
    event.estimate = latest_;
    event.command_accepted = true;
    enqueue(event);
}

inline Session::Stats Session::stats() const noexcept {
    return {samples_.load(),   read_errors_.load(), written_.load(),          dropped_.load(),
            io_errors_.load(), log_bytes_.load(),   log_limit_reached_.load()};
}

inline void Session::enqueue(Event event) noexcept {
    if (!accepting_)
        return;
    event.sequence = ++sequence_;
    event.recorded_ns = now_ns();
    if (log_limit_reached_.load()) {
        ++dropped_;
        return;
    }
    {
        // The writer only holds this lock to copy a single event, never for I/O.
        std::lock_guard lock(queue_mutex_);
        if (count_ == kQueueCapacity) {
            ++dropped_;
            return;
        }
        event.dropped = dropped_.load();
        queue_[tail_] = event;
        tail_ = (tail_ + 1) % kQueueCapacity;
        ++count_;
    }
    queue_ready_.notify_one();
}

inline void Session::sample_loop() noexcept {
    auto next = std::chrono::steady_clock::now();
    while (sampling_.load()) {
        const auto sample = source_.read_sample();
        {
            std::lock_guard lock(state_mutex_);
            Event event{};
            if (sample) {
                ++samples_;
                latest_ = estimator_.update(*sample);
                event.estimate = latest_;
                enqueue(event);
                if (initial_seed_pending_) {
                    const auto seeded = seed_locked({});
                    initial_seed_pending_ = !seeded.accepted;
                }
            } else {
                ++read_errors_;
                event.kind = EventKind::kReadError;
                event.estimate = latest_;
                event.read_error = static_cast<int>(sample.error());
                enqueue(event);
            }
        }
        next += std::chrono::microseconds{period_us_};
        const auto now = std::chrono::steady_clock::now();
        if (next < now)
            next = now + std::chrono::microseconds{period_us_};
        std::unique_lock lock(sleep_mutex_);
        sample_wake_.wait_until(lock, next, [this] { return !sampling_.load(); });
    }
}

inline void Session::writer_loop() noexcept {
    auto next_flush = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    for (;;) {
        Event event{};
        {
            std::unique_lock lock(queue_mutex_);
            queue_ready_.wait_until(
                lock, next_flush, [this] { return count_ > 0 || writer_stopping_; });
            if (count_ == 0 && writer_stopping_)
                break;
            if (count_ > 0) {
                event = queue_[head_];
                head_ = (head_ + 1) % kQueueCapacity;
                --count_;
            }
        }
        if (event.sequence != 0) {
            if (write_event(event))
                ++written_;
            else if (log_limit_reached_.load())
                ++dropped_;
            else
                ++io_errors_;
        }
        if (std::chrono::steady_clock::now() >= next_flush) {
            if (std::fflush(file_) != 0)
                ++io_errors_;
            if (::fsync(::fileno(file_)) != 0)
                ++io_errors_;
            next_flush = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        }
    }
}

inline bool Session::write_event(const Event& event) noexcept {
    if (log_limit_reached_.load())
        return false;
    const auto& e = event.estimate;
    const auto& s = e.sample;
    const auto& a = s.raw_counts.accelerometer;
    const auto& g = s.raw_counts.gyroscope;
    const auto& q = e.attitude;
    const auto& r = event.requested_reference;
    const char* kind = "sample";
    switch (event.kind) {
    case EventKind::kSample: break;
    case EventKind::kSeed: kind = "seed"; break;
    case EventKind::kReset: kind = "reset"; break;
    case EventKind::kReadError: kind = "read_error"; break;
    }
    char row[2048];
    const int length = std::snprintf(
        row, sizeof(row),
        "%s,%llu,%llu,%llu,%llu,%u,%d,%d,%d,%d,%u,%d,%llu,"
        "%d,%d,%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,"
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
        kind, static_cast<unsigned long long>(event.sequence),
        static_cast<unsigned long long>(event.recorded_ns),
        static_cast<unsigned long long>(s.timestamp_ns),
        static_cast<unsigned long long>(e.generation), e.quality_events, e.sample_accepted,
        e.integrated, e.has_reference_frame, event.command_accepted, event.command_quality_events,
        event.read_error, static_cast<unsigned long long>(event.dropped), a.x, a.y, a.z, g.x, g.y,
        g.z, s.acceleration_m_s2.x, s.acceleration_m_s2.y, s.acceleration_m_s2.z,
        s.angular_velocity_rad_s.x, s.angular_velocity_rad_s.y, s.angular_velocity_rad_s.z,
        s.temperature_c, s.flags, q.w, q.x, q.y, q.z, e.attitude_rpy_rad.x, e.attitude_rpy_rad.y,
        e.attitude_rpy_rad.z, e.angular_velocity_rad_s.x, e.angular_velocity_rad_s.y,
        e.angular_velocity_rad_s.z, e.linear_acceleration_m_s2.x, e.linear_acceleration_m_s2.y,
        e.linear_acceleration_m_s2.z, e.velocity_m_s.x, e.velocity_m_s.y, e.velocity_m_s.z,
        e.cumulative_rotation_rad.x, e.cumulative_rotation_rad.y, e.cumulative_rotation_rad.z, r.w,
        r.x, r.y, r.z);
    return write_log(row, length, sizeof(row));
}

inline bool Session::write_log(const char* data, int length, std::size_t capacity) noexcept {
    if (log_limit_reached_.load())
        return false;
    if (length < 0 || static_cast<std::size_t>(length) >= capacity)
        return false;
    const auto bytes = static_cast<std::uint64_t>(length);
    const auto used = log_bytes_.load();
    // The cap applies across application restarts; preserve existing data.
    if (used >= log_limit_bytes_ || bytes > log_limit_bytes_ - used) {
        log_limit_reached_.store(true);
        return false;
    }
    const auto written = std::fwrite(data, 1, static_cast<std::size_t>(length), file_);
    log_bytes_.fetch_add(written);
    return written == static_cast<std::size_t>(length);
}

} // namespace dart::imu

#endif // DART_RUNTIME_IMU_SESSION_CPP
