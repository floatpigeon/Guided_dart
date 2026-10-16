#ifndef DART_RUNTIME_RECORDOR_RECORDER_CPP
#define DART_RUNTIME_RECORDOR_RECORDER_CPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <pthread.h>
#include <string_view>
#include <utility>

#include "camera/frame_lease.cpp"
#include "config.cpp"
#include "error.cpp"
#include "event_writer.cpp"
#include "image_writer.cpp"
#include "session.cpp"

namespace dart::recorder {

enum class Admission : std::uint8_t {
    kAccepted,
    kDownsampled,
    kBusy,
    kStopped,
    kInvalidFrame,
};

class Recorder final {
public:
    struct Stats {
        std::uint64_t input = 0;
        std::uint64_t accepted = 0;
        std::uint64_t copied = 0;
        std::uint64_t written = 0;
        std::uint64_t downsampled = 0;
        std::uint64_t busy = 0;
        std::uint64_t stopped = 0;
        std::uint64_t invalid = 0;
        std::uint64_t copy_fail = 0;
        std::uint64_t write_fail = 0;
        std::uint64_t event_dropped = 0;
        std::uint64_t lease_release_fail = 0;
        std::uint64_t bytes = 0;
        std::uint32_t queued = 0;
        std::uint32_t release_pending = 0;
        std::uint32_t queue_high_water = 0;
    };

    explicit Recorder(Config config) noexcept;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool start() noexcept;
    void stop() noexcept;

    /* On kAccepted ownership moves to Recorder. Otherwise the caller retains
     * the lease and releases it in the acquisition thread. This never waits. */
    Admission try_submit(camera::FrameLease&& lease) noexcept;
    Admission submit(camera::FrameLease&& lease) noexcept { return try_submit(std::move(lease)); }

    /* Camera release is serialized here with acquisition. Poll every capture
     * iteration and once after stop(), before destroying the camera. */
    std::uint32_t drain_releases() noexcept;
    std::uint32_t pending_releases() const noexcept;

    bool started() const noexcept;
    Error last_error() const noexcept { return error_.last_error(); }
    int last_system_error() const noexcept { return error_.last_system_error(); }
    const char* last_error_path() const noexcept { return error_.last_error_path(); }
    static std::string_view error_text(Error error) noexcept;
    static std::string_view decision_text(EventDecision decision) noexcept {
        return EventWriter::decision_text(decision);
    }
    Stats stats() const noexcept;

private:
    struct ImageSlot {
        std::unique_ptr<std::byte[]> pixels{};
        camera::FrameLease lease{};
        FrameEvent event{};
    };

    static constexpr std::uint64_t kMaxFrameDimension = 16384;

    static bool checked_frame_bytes(
        std::uint32_t width, std::uint32_t height, std::size_t& bytes) noexcept {
        if (width == 0 || height == 0 || width > kMaxFrameDimension || height > kMaxFrameDimension)
            return false;
        const auto w = static_cast<std::size_t>(width);
        const auto h = static_cast<std::size_t>(height);
        if (h > std::numeric_limits<std::size_t>::max() / w)
            return false;
        bytes = w * h;
        return true;
    }

    static constexpr std::size_t kMaxPath = 512;
    static constexpr std::uint32_t kMaxQueueSlots = 64;
    static constexpr std::uint32_t kMaxEventSlots = 4096;

    bool validate_config() noexcept;
    bool allocate_buffers() noexcept;

    bool enqueue_event(const FrameEvent& event) noexcept;
    void image_loop() noexcept;
    void event_loop() noexcept;
    /* pthread entry points.  This module builds under -fno-exceptions, where a
     * throwing std::thread constructor has no recoverable error path, so thread
     * creation reports failure through pthread_create's return code instead. */
    static void* image_thread_entry(void* context) noexcept;
    static void* event_thread_entry(void* context) noexcept;
    void rollback_start() noexcept;

    void set_error(Error error) noexcept { error_.set_error(error); }

    Config config_{};
    RecorderError error_{};
    Session session_{config_, error_};
    ImageWriter images_{config_, session_};
    EventWriter events_{config_};
    std::size_t frame_bytes_ = 0;

    std::unique_ptr<ImageSlot[]> image_slots_{};
    std::unique_ptr<camera::FrameLease[]> release_slots_{};
    std::unique_ptr<FrameEvent[]> event_slots_{};

    std::uint32_t image_capacity_ = 0;
    std::uint32_t event_capacity_ = 0;
    std::uint32_t image_head_ = 0;
    std::uint32_t image_tail_ = 0;
    std::uint32_t image_count_ = 0;
    std::uint32_t release_head_ = 0;
    std::uint32_t release_tail_ = 0;
    std::uint32_t release_count_ = 0;
    std::uint32_t event_head_ = 0;
    std::uint32_t event_tail_ = 0;
    std::uint32_t event_count_ = 0;
    std::uint64_t next_image_index_ = 1;
    mutable std::mutex image_mutex_;
    std::condition_variable image_cv_;
    std::mutex event_mutex_;
    std::condition_variable event_cv_;
    pthread_t image_thread_{};
    pthread_t event_thread_{};
    bool image_thread_started_ = false;
    bool event_thread_started_ = false;
    bool accepting_ = false;
    /* A Recorder owns camera leases across its worker and release queue.  A
     * second start would otherwise reset those queues and lose unreleased
     * leases, so a successfully started instance is single-use. */
    std::atomic<bool> started_once_{false};
    bool event_running_ = false;

    std::atomic<std::uint64_t> input_{0};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> copied_{0};
    std::atomic<std::uint64_t> written_{0};
    std::atomic<std::uint64_t> downsampled_{0};
    std::atomic<std::uint64_t> busy_{0};
    std::atomic<std::uint64_t> stopped_{0};
    std::atomic<std::uint64_t> invalid_{0};
    std::atomic<std::uint64_t> copy_fail_{0};
    std::atomic<std::uint64_t> write_fail_{0};
    std::atomic<std::uint64_t> event_dropped_{0};
    std::atomic<std::uint64_t> lease_release_fail_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> next_sample_tick_{0};
    std::atomic<std::uint32_t> queue_high_water_{0};
};

inline Recorder::Recorder(Config config) noexcept
    : config_(config) {}

inline Recorder::~Recorder() {
    stop();
    if (image_slots_) {
        for (std::uint32_t i = 0; i < image_capacity_; ++i)
            image_slots_[i].lease = camera::FrameLease{};
    }
    if (release_slots_) {
        for (std::uint32_t i = 0; i < image_capacity_; ++i)
            release_slots_[i] = camera::FrameLease{};
    }
}

inline bool Recorder::validate_config() noexcept {
    if (!checked_frame_bytes(config_.width, config_.height, frame_bytes_)) {
        set_error(Error::kInvalidConfig);
        return false;
    }
    if (config_.fps == 0 || config_.fps > 1000000 || config_.queue_slots == 0
        || config_.queue_slots > kMaxQueueSlots || config_.event_slots == 0
        || config_.event_slots > kMaxEventSlots || config_.output_directory.empty()
        || config_.output_directory.size() >= kMaxPath || config_.flush_period.count() < 0
        || config_.keep_sessions > 9999) {
        set_error(
            config_.output_directory.size() >= kMaxPath ? Error::kPathTooLong
                                                        : Error::kInvalidConfig);
        return false;
    }
    return true;
}

inline bool Recorder::allocate_buffers() noexcept {
    image_capacity_ = config_.queue_slots;
    event_capacity_ = config_.event_slots;
    image_slots_.reset(new (std::nothrow) ImageSlot[image_capacity_]);
    release_slots_.reset(new (std::nothrow) camera::FrameLease[image_capacity_]);
    event_slots_.reset(new (std::nothrow) FrameEvent[event_capacity_]);
    if (!image_slots_ || !release_slots_ || !event_slots_) {
        set_error(Error::kNoMemory);
        return false;
    }
    for (std::uint32_t i = 0; i < image_capacity_; ++i) {
        image_slots_[i].pixels.reset(new (std::nothrow) std::byte[frame_bytes_]);
        if (!image_slots_[i].pixels) {
            set_error(Error::kNoMemory);
            return false;
        }
    }
    if (!images_.prepare(frame_bytes_) || !events_.prepare()) {
        set_error(Error::kNoMemory);
        return false;
    }
    return true;
}

inline bool Recorder::start() noexcept {
    if (started() || started_once_.load(std::memory_order_acquire)) {
        set_error(Error::kAlreadyStarted);
        return false;
    }
    error_.reset();
    if (!validate_config() || !allocate_buffers()
        || (!session_.open() || !events_.open(session_, error_))) {
        events_.close();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(image_mutex_);
        image_head_ = image_tail_ = image_count_ = 0;
        release_head_ = release_tail_ = release_count_ = 0;
        next_image_index_ = 1;
        next_sample_tick_.store(0, std::memory_order_relaxed);
        accepting_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_head_ = event_tail_ = event_count_ = 0;
        event_running_ = true;
    }
    if (::pthread_create(&image_thread_, nullptr, &Recorder::image_thread_entry, this) != 0) {
        rollback_start();
        return false;
    }
    image_thread_started_ = true;
    if (::pthread_create(&event_thread_, nullptr, &Recorder::event_thread_entry, this) != 0) {
        rollback_start();
        return false;
    }
    event_thread_started_ = true;
    started_once_.store(true, std::memory_order_release);
    return true;
}

inline void Recorder::rollback_start() noexcept {
    set_error(Error::kThread);
    {
        std::lock_guard<std::mutex> lock(image_mutex_);
        accepting_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_running_ = false;
    }
    image_cv_.notify_all();
    event_cv_.notify_all();
    if (image_thread_started_) {
        (void)::pthread_join(image_thread_, nullptr);
        image_thread_started_ = false;
    }
    if (event_thread_started_) {
        (void)::pthread_join(event_thread_, nullptr);
        event_thread_started_ = false;
    }
    events_.close();
}

inline void Recorder::stop() noexcept {
    if (!started() && !image_thread_started_ && !event_thread_started_)
        return;
    {
        std::lock_guard<std::mutex> lock(image_mutex_);
        accepting_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_running_ = false;
    }
    image_cv_.notify_all();
    event_cv_.notify_all();
    if (image_thread_started_) {
        (void)::pthread_join(image_thread_, nullptr);
        image_thread_started_ = false;
    }
    if (event_thread_started_) {
        (void)::pthread_join(event_thread_, nullptr);
        event_thread_started_ = false;
    }
    events_.close();
}

inline bool Recorder::started() const noexcept {
    std::lock_guard<std::mutex> lock(image_mutex_);
    return accepting_;
}

inline Admission Recorder::try_submit(camera::FrameLease&& lease) noexcept {
    const std::uint64_t input_index = input_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto* view_ptr = lease.valid() ? &lease.view() : nullptr;
    FrameEvent event{};
    event.input_index = input_index;
    if (view_ptr != nullptr) {
        event.source_sequence = view_ptr->sequence;
        event.source_pts = view_ptr->source_pts;
        event.received_us = view_ptr->received.count();
        event.source_pts_timebase_validated = view_ptr->source_pts_timebase_validated;
    }

    const auto finish = [&](Admission admission, EventDecision decision) noexcept {
        event.decision = decision;
        if (admission == Admission::kAccepted)
            accepted_.fetch_add(1, std::memory_order_relaxed);
        else if (admission == Admission::kDownsampled)
            downsampled_.fetch_add(1, std::memory_order_relaxed);
        else if (admission == Admission::kBusy)
            busy_.fetch_add(1, std::memory_order_relaxed);
        else if (admission == Admission::kStopped)
            stopped_.fetch_add(1, std::memory_order_relaxed);
        else if (admission == Admission::kInvalidFrame)
            invalid_.fetch_add(1, std::memory_order_relaxed);
        (void)enqueue_event(event);
        return admission;
    };

    if (view_ptr == nullptr)
        return finish(Admission::kInvalidFrame, EventDecision::kInvalidFrame);
    if (!started_once_.load(std::memory_order_acquire))
        return finish(Admission::kStopped, EventDecision::kStopped);
    if (!images_.valid_view(*view_ptr) || event.received_us < 0)
        return finish(Admission::kInvalidFrame, EventDecision::kInvalidFrame);

    /* One image per 1/fps CLOCK_MONOTONIC interval, independent of input FPS.
     * Split seconds and microseconds to avoid overflow and integer-period drift.
     * Advance before the queue check: missed/busy intervals are never caught up
     * by a burst of writes when capture or storage resumes. */
    const auto received_us = static_cast<std::uint64_t>(event.received_us);
    const std::uint64_t tick =
        (received_us / 1000000U) * config_.fps + (received_us % 1000000U) * config_.fps / 1000000U;
    auto next_tick = next_sample_tick_.load(std::memory_order_relaxed);
    if (tick < next_tick
        || !next_sample_tick_.compare_exchange_strong(
            next_tick, tick + 1U, std::memory_order_relaxed))
        return finish(Admission::kDownsampled, EventDecision::kDownsampled);

    /* No condition-variable wait, and even the queue mutex is try-locked. */
    std::unique_lock<std::mutex> lock(image_mutex_, std::try_to_lock);
    /* image_count_ includes a slot being copied/written.  release_count_ is
     * also part of the hard bound: until the acquisition thread drains a
     * lease, that slot is still owned by Recorder and cannot be reused. */
    if (!lock.owns_lock())
        return finish(Admission::kBusy, EventDecision::kBusy);
    const bool full = image_count_ + release_count_ >= image_capacity_;
    if (!accepting_ || full) {
        return finish(
            !accepting_ ? Admission::kStopped : Admission::kBusy,
            !accepting_ ? EventDecision::kStopped : EventDecision::kBusy);
    }
    ImageSlot& slot = image_slots_[image_tail_];
    slot.event = event;
    slot.event.decision = EventDecision::kAccepted;
    slot.event.image_index = next_image_index_++;
    (void)std::snprintf(
        slot.event.image_name.data(), slot.event.image_name.size(), "img/frame_%08llu.%s",
        static_cast<unsigned long long>(slot.event.image_index),
        config_.image_format == ImageFormat::kPbm ? "pbm" : "pgm");
    slot.lease = std::move(lease);
    image_tail_ = (image_tail_ + 1U) % image_capacity_;
    ++image_count_;
    const auto held = image_count_ + release_count_;
    const auto high = std::max(queue_high_water_.load(std::memory_order_relaxed), held);
    queue_high_water_.store(high, std::memory_order_relaxed);
    event = slot.event;
    lock.unlock();
    (void)enqueue_event(event);
    accepted_.fetch_add(1, std::memory_order_relaxed);
    image_cv_.notify_one();
    return Admission::kAccepted;
}

inline bool Recorder::enqueue_event(const FrameEvent& event) noexcept {
    std::unique_lock<std::mutex> lock(event_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !event_running_ || event_count_ >= event_capacity_) {
        event_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    FrameEvent& slot = event_slots_[event_tail_];
    slot = event;
    event_tail_ = (event_tail_ + 1U) % event_capacity_;
    ++event_count_;
    lock.unlock();
    event_cv_.notify_one();
    return true;
}

inline std::uint32_t Recorder::drain_releases() noexcept {
    std::uint32_t drained = 0;
    for (;;) {
        camera::FrameLease lease{};
        {
            std::unique_lock<std::mutex> lock(image_mutex_, std::try_to_lock);
            if (!lock.owns_lock() || release_count_ == 0)
                break;
            camera::FrameLease& slot = release_slots_[release_head_];
            lease = std::move(slot);
            release_head_ = (release_head_ + 1U) % image_capacity_;
            --release_count_;
        }
        if (lease.valid()) {
            auto released = lease.release();
            if (!released)
                lease_release_fail_.fetch_add(1, std::memory_order_relaxed);
        }
        ++drained;
    }
    return drained;
}

inline std::uint32_t Recorder::pending_releases() const noexcept {
    std::lock_guard<std::mutex> lock(image_mutex_);
    return release_count_;
}

inline void* Recorder::image_thread_entry(void* context) noexcept {
    static_cast<Recorder*>(context)->image_loop();
    return nullptr;
}

inline void* Recorder::event_thread_entry(void* context) noexcept {
    static_cast<Recorder*>(context)->event_loop();
    return nullptr;
}

inline void Recorder::image_loop() noexcept {
    for (;;) {
        ImageSlot* slot = nullptr;
        {
            std::unique_lock<std::mutex> lock(image_mutex_);
            image_cv_.wait(lock, [this] { return image_count_ != 0 || !accepting_; });
            if (image_count_ == 0 && !accepting_)
                break;
            slot = &image_slots_[image_head_];
        }
        const auto view = slot->lease.view();
        const bool copied = images_.copy_image({slot->pixels.get(), frame_bytes_}, view);
        if (copied)
            copied_.fetch_add(1, std::memory_order_relaxed);
        else
            copy_fail_.fetch_add(1, std::memory_order_relaxed);
        camera::FrameLease source_lease = std::move(slot->lease);
        if (copied) {
            if (images_.write_image(
                    {slot->pixels.get(), frame_bytes_}, slot->event.image_name.data())) {
                written_.fetch_add(1, std::memory_order_relaxed);
                bytes_.fetch_add(images_.payload_bytes(), std::memory_order_relaxed);
            } else {
                write_fail_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        {
            std::lock_guard<std::mutex> lock(image_mutex_);
            camera::FrameLease& release = release_slots_[release_tail_];
            release = std::move(source_lease);
            release_tail_ = (release_tail_ + 1U) % image_capacity_;
            ++release_count_;
            image_head_ = (image_head_ + 1U) % image_capacity_;
            --image_count_;
        }
        image_cv_.notify_all();
    }
}

inline void Recorder::event_loop() noexcept {
    for (;;) {
        FrameEvent event{};
        {
            std::unique_lock<std::mutex> lock(event_mutex_);
            event_cv_.wait(lock, [this] { return event_count_ != 0 || !event_running_; });
            if (event_count_ == 0 && !event_running_)
                break;
            FrameEvent& slot = event_slots_[event_head_];
            event = slot;
            event_head_ = (event_head_ + 1U) % event_capacity_;
            --event_count_;
        }
        events_.write(event);
    }
    events_.maybe_flush_events(true);
}

inline std::string_view Recorder::error_text(Error error) noexcept {
    switch (error) {
    case Error::kNone: return "none";
    case Error::kAlreadyStarted: return "already_started";
    case Error::kInvalidConfig: return "invalid_config";
    case Error::kNoMemory: return "no_memory";
    case Error::kPathTooLong: return "path_too_long";
    case Error::kDirectory: return "directory";
    case Error::kEventFile: return "event_file";
    case Error::kThread: return "thread";
    }
    return "unknown";
}

inline Recorder::Stats Recorder::stats() const noexcept {
    Stats result{};
    result.input = input_.load(std::memory_order_relaxed);
    result.accepted = accepted_.load(std::memory_order_relaxed);
    result.copied = copied_.load(std::memory_order_relaxed);
    result.written = written_.load(std::memory_order_relaxed);
    result.downsampled = downsampled_.load(std::memory_order_relaxed);
    result.busy = busy_.load(std::memory_order_relaxed);
    result.stopped = stopped_.load(std::memory_order_relaxed);
    result.invalid = invalid_.load(std::memory_order_relaxed);
    result.copy_fail = copy_fail_.load(std::memory_order_relaxed);
    result.write_fail = write_fail_.load(std::memory_order_relaxed);
    result.event_dropped = event_dropped_.load(std::memory_order_relaxed);
    result.lease_release_fail = lease_release_fail_.load(std::memory_order_relaxed);
    result.bytes = bytes_.load(std::memory_order_relaxed);
    result.queue_high_water = queue_high_water_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(image_mutex_);
        result.queued = image_count_;
        result.release_pending = release_count_;
    }
    return result;
}

} // namespace dart::recorder

#endif // DART_RUNTIME_RECORDOR_RECORDER_CPP
