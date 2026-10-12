/* 录像垂直切片：声明与实现合并为单个翻译单元（原 recorder.hpp 已并入本文件）。
 *
 * 只接收相机 ACL 的 FrameLease，不含 K230/MPP/VENC 头文件；输入帧在写线程中
 * 拷贝为紧凑 Y8，再写成 PGM（或阈值打包后的 PBM）。硬件 H.264 需要后续独立的
 * video ACL，不会把厂商句柄带入这个上层模块。
 *
 * 与 detection 模块一致，本文件自带重复包含保护：应用既可以 #include 本 .cpp
 * 直接引入完整实现，也可以链接单独编译本文件的 dart_recording 目标。
 */
#ifndef DART_RECORDING_RECORDER_CPP
#define DART_RECORDING_RECORDER_CPP

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include "camera/camera.hpp"

namespace dart::recorder {

enum class ImageFormat : std::uint8_t { kPgm, kPbm };

enum class Admission : std::uint8_t {
    kAccepted,
    kDownsampled,
    kBusy,
    kStopped,
    kInvalidFrame,
};

enum class EventDecision : std::uint8_t {
    kAccepted,
    kDownsampled,
    kBusy,
    kStopped,
    kInvalidFrame,
};

struct FrameEvent {
    std::uint64_t input_index = 0;
    std::uint64_t source_sequence = 0;
    std::uint64_t source_pts = 0;
    std::int64_t received_us = 0;
    bool source_pts_timebase_validated = false;
    std::uint64_t image_index = 0;
    EventDecision decision = EventDecision::kStopped;
    std::array<char, 48> image_name{};
};

using FrameEventSink = void (*)(const FrameEvent& event, void* context) noexcept;

enum class Error : std::uint8_t {
    kNone,
    kAlreadyStarted,
    kInvalidConfig,
    kNoMemory,
    kPathTooLong,
    kDirectory,
    kEventFile,
    kThread,
};

struct Config {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /* Sampling is checked before queue availability: 240 Hz / 4 ~= 60 Hz. */
    std::uint32_t sample_every_n = 4;
    /* Hard image-slot cap; a slot stays busy through copy and file write. */
    std::uint32_t queue_slots = 2;
    /* Separate bounded metadata queue for one CSV row per enqueued submission; drops
     * are counted instead of blocking capture. */
    std::uint32_t event_slots = 512;
    ImageFormat image_format = ImageFormat::kPgm;
    std::uint8_t pbm_threshold = 128;
    std::chrono::milliseconds flush_period{1000};
    std::string_view output_directory = "records";
    FrameEventSink event_sink = nullptr;
    void* event_sink_context = nullptr;
};

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

class Recorder final {
public:
    explicit Recorder(Config config) noexcept;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool start() noexcept;
    void stop() noexcept;

    /* On kAccepted ownership moves to Recorder. Otherwise the caller retains
     * the lease and releases it in the acquisition thread. This never waits. */
    Admission try_submit(camera::FrameLease&& lease) noexcept;
    Admission submit(camera::FrameLease&& lease) noexcept {
        return try_submit(std::move(lease));
    }

    /* Camera release is serialized here with acquisition. Poll every capture
     * iteration and once after stop(), before destroying the camera. */
    std::uint32_t drain_releases() noexcept;
    std::uint32_t pending_releases() const noexcept;

    bool started() const noexcept;
    Error last_error() const noexcept { return last_error_.load(std::memory_order_acquire); }
    static std::string_view error_text(Error error) noexcept;
    static std::string_view decision_text(EventDecision decision) noexcept;
    Stats stats() const noexcept;

private:
    struct ImageSlot {
        std::unique_ptr<std::byte[]> pixels{};
        camera::FrameLease lease{};
        FrameEvent event{};
        bool occupied = false;
    };
    struct ReleaseSlot {
        camera::FrameLease lease{};
        bool occupied = false;
    };
    struct EventSlot {
        FrameEvent event{};
        bool occupied = false;
    };

    static constexpr std::size_t kMaxPath = 512;
    static constexpr std::uint32_t kMaxQueueSlots = 64;
    static constexpr std::uint32_t kMaxEventSlots = 4096;

    bool validate_config() noexcept;
    bool allocate_buffers() noexcept;
    bool open_session() noexcept;
    void close_session() noexcept;
    bool valid_view(const camera::FrameView& view) const noexcept;
    bool enqueue_event(const FrameEvent& event) noexcept;
    void image_loop() noexcept;
    void event_loop() noexcept;
    /* pthread entry points.  This module builds under -fno-exceptions, where a
     * throwing std::thread constructor has no recoverable error path, so thread
     * creation reports failure through pthread_create's return code instead. */
    static void* image_thread_entry(void* context) noexcept;
    static void* event_thread_entry(void* context) noexcept;
    void rollback_start() noexcept;
    bool copy_image(ImageSlot& slot, const camera::FrameView& view) noexcept;
    bool write_image(const ImageSlot& slot) const noexcept;
    bool write_pgm(const ImageSlot& slot, const char* path) const noexcept;
    bool write_pbm(const ImageSlot& slot, const char* path) const noexcept;
    void maybe_flush_events(bool force) noexcept;
    void set_error(Error error) noexcept {
        last_error_.store(error, std::memory_order_release);
    }

    Config config_{};
    std::size_t frame_bytes_ = 0;
    std::size_t pbm_row_bytes_ = 0;
    std::unique_ptr<ImageSlot[]> image_slots_{};
    std::unique_ptr<ReleaseSlot[]> release_slots_{};
    std::unique_ptr<EventSlot[]> event_slots_{};
    std::unique_ptr<std::byte[]> pbm_row_{};
    std::unique_ptr<char[]> event_buffer_{};
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
    char session_dir_[kMaxPath]{};
    char image_dir_[kMaxPath]{};
    char event_path_[kMaxPath]{};
    std::FILE* event_file_ = nullptr;
    std::chrono::steady_clock::time_point last_event_flush_{};
    std::atomic<Error> last_error_{Error::kNone};
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
    std::atomic<std::uint64_t> sample_counter_{0};
    std::atomic<std::uint32_t> queue_high_water_{0};
};

namespace {

constexpr std::uint64_t kMaxFrameDimension = 16384;
constexpr std::size_t kEventBufferBytes = 64U * 1024U;

bool mkdir_if_needed(const char* path) noexcept {
    if (::mkdir(path, 0777) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat st{};
    return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool checked_frame_bytes(std::uint32_t width, std::uint32_t height,
                         std::size_t& bytes) noexcept {
    if (width == 0 || height == 0 || width > kMaxFrameDimension || height > kMaxFrameDimension)
        return false;
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (h > std::numeric_limits<std::size_t>::max() / w) return false;
    bytes = w * h;
    return true;
}

const char* extension(ImageFormat format) noexcept {
    return format == ImageFormat::kPbm ? "pbm" : "pgm";
}

}  // namespace

Recorder::Recorder(Config config) noexcept : config_(config) {}

Recorder::~Recorder() {
    stop();
    if (image_slots_) {
        for (std::uint32_t i = 0; i < image_capacity_; ++i)
            image_slots_[i].lease = camera::FrameLease{};
    }
    if (release_slots_) {
        for (std::uint32_t i = 0; i < image_capacity_; ++i)
            release_slots_[i].lease = camera::FrameLease{};
    }
}

bool Recorder::validate_config() noexcept {
    if (!checked_frame_bytes(config_.width, config_.height, frame_bytes_)) {
        set_error(Error::kInvalidConfig);
        return false;
    }
    if (config_.sample_every_n == 0 || config_.queue_slots == 0 ||
        config_.queue_slots > kMaxQueueSlots || config_.event_slots == 0 ||
        config_.event_slots > kMaxEventSlots || config_.output_directory.empty() ||
        config_.output_directory.size() >= kMaxPath || config_.flush_period.count() < 0) {
        set_error(config_.output_directory.size() >= kMaxPath ? Error::kPathTooLong
                                                               : Error::kInvalidConfig);
        return false;
    }
    if (config_.image_format == ImageFormat::kPbm)
        pbm_row_bytes_ = (static_cast<std::size_t>(config_.width) + 7U) / 8U;
    return true;
}

bool Recorder::allocate_buffers() noexcept {
    image_capacity_ = config_.queue_slots;
    event_capacity_ = config_.event_slots;
    image_slots_.reset(new (std::nothrow) ImageSlot[image_capacity_]);
    release_slots_.reset(new (std::nothrow) ReleaseSlot[image_capacity_]);
    event_slots_.reset(new (std::nothrow) EventSlot[event_capacity_]);
    event_buffer_.reset(new (std::nothrow) char[kEventBufferBytes]);
    if (!image_slots_ || !release_slots_ || !event_slots_ || !event_buffer_) {
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
    if (config_.image_format == ImageFormat::kPbm) {
        pbm_row_.reset(new (std::nothrow) std::byte[pbm_row_bytes_]);
        if (!pbm_row_) {
            set_error(Error::kNoMemory);
            return false;
        }
    }
    return true;
}

bool Recorder::open_session() noexcept {
    const auto root = config_.output_directory;
    char root_path[kMaxPath]{};
    std::memcpy(root_path, root.data(), root.size());
    root_path[root.size()] = '\0';
    if (!mkdir_if_needed(root_path)) {
        set_error(Error::kDirectory);
        return false;
    }
    bool created = false;
    for (std::uint32_t i = 1; i <= 9999; ++i) {
        const int n = std::snprintf(session_dir_, sizeof(session_dir_), "%.*s/rec_%04u",
                                    static_cast<int>(root.size()), root.data(), i);
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(session_dir_)) {
            set_error(Error::kPathTooLong);
            return false;
        }
        if (::access(session_dir_, F_OK) == 0) continue;
        if (!mkdir_if_needed(session_dir_)) {
            set_error(Error::kDirectory);
            return false;
        }
        created = true;
        break;
    }
    if (!created) {
        set_error(Error::kDirectory);
        return false;
    }

    char log_dir[kMaxPath]{};
    const int image_n = std::snprintf(image_dir_, sizeof(image_dir_), "%s/img", session_dir_);
    const int log_n = std::snprintf(log_dir, sizeof(log_dir), "%s/logs", session_dir_);
    const int event_n = std::snprintf(event_path_, sizeof(event_path_), "%s/frames.csv", log_dir);
    if (image_n <= 0 || log_n <= 0 || event_n <= 0 ||
        static_cast<std::size_t>(image_n) >= sizeof(image_dir_) ||
        static_cast<std::size_t>(log_n) >= sizeof(log_dir) ||
        static_cast<std::size_t>(event_n) >= sizeof(event_path_) ||
        !mkdir_if_needed(log_dir) || !mkdir_if_needed(image_dir_)) {
        set_error(Error::kDirectory);
        return false;
    }
    event_file_ = std::fopen(event_path_, "w");
    if (event_file_ == nullptr) {
        set_error(Error::kEventFile);
        return false;
    }
    if (event_buffer_)
        (void)std::setvbuf(event_file_, event_buffer_.get(), _IOFBF, kEventBufferBytes);
    std::fprintf(event_file_,
                 "# recorder format=1 width=%u height=%u sample_every_n=%u session=%s\n"
                 "input_index,source_sequence,source_pts,source_pts_timebase_validated,received_us,decision,image_index,image_file\n",
                 config_.width, config_.height, config_.sample_every_n, session_dir_);
    std::fflush(event_file_);
    last_event_flush_ = std::chrono::steady_clock::now();
    return true;
}

void Recorder::close_session() noexcept {
    if (event_file_ != nullptr) {
        maybe_flush_events(true);
        std::fclose(event_file_);
        event_file_ = nullptr;
    }
}

bool Recorder::start() noexcept {
    if (started() || started_once_.load(std::memory_order_acquire)) {
        set_error(Error::kAlreadyStarted);
        return false;
    }
    set_error(Error::kNone);
    if (!validate_config() || !allocate_buffers() || !open_session()) {
        close_session();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(image_mutex_);
        image_head_ = image_tail_ = image_count_ = 0;
        release_head_ = release_tail_ = release_count_ = 0;
        next_image_index_ = 1;
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

void Recorder::rollback_start() noexcept {
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
    close_session();
}

void Recorder::stop() noexcept {
    if (!started() && !image_thread_started_ && !event_thread_started_) return;
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
    close_session();
}

bool Recorder::started() const noexcept {
    std::lock_guard<std::mutex> lock(image_mutex_);
    return accepting_;
}

bool Recorder::valid_view(const camera::FrameView& view) const noexcept {
    if (view.width != config_.width || view.height != config_.height || view.stride < view.width ||
        view.luma.data() == nullptr) return false;
    const std::uint64_t required =
        static_cast<std::uint64_t>(view.height - 1U) * view.stride + view.width;
    return required <= view.luma.size();
}

Admission Recorder::try_submit(camera::FrameLease&& lease) noexcept {
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
        if (admission == Admission::kAccepted) accepted_.fetch_add(1, std::memory_order_relaxed);
        else if (admission == Admission::kDownsampled) downsampled_.fetch_add(1, std::memory_order_relaxed);
        else if (admission == Admission::kBusy) busy_.fetch_add(1, std::memory_order_relaxed);
        else if (admission == Admission::kStopped) stopped_.fetch_add(1, std::memory_order_relaxed);
        else if (admission == Admission::kInvalidFrame) invalid_.fetch_add(1, std::memory_order_relaxed);
        (void)enqueue_event(event);
        return admission;
    };

    if (view_ptr == nullptr) return finish(Admission::kInvalidFrame, EventDecision::kInvalidFrame);
    const std::uint64_t ordinal = sample_counter_.fetch_add(1, std::memory_order_relaxed);
    if ((ordinal % config_.sample_every_n) != 0)
        return finish(Admission::kDownsampled, EventDecision::kDownsampled);
    if (!valid_view(*view_ptr)) return finish(Admission::kInvalidFrame, EventDecision::kInvalidFrame);

    /* No condition-variable wait, and even the queue mutex is try-locked. */
    std::unique_lock<std::mutex> lock(image_mutex_, std::try_to_lock);
    /* image_count_ includes a slot being copied/written.  release_count_ is
     * also part of the hard bound: until the acquisition thread drains a
     * lease, that slot is still owned by Recorder and cannot be reused. */
    if (!lock.owns_lock())
        return finish(Admission::kBusy, EventDecision::kBusy);
    const bool full = image_count_ + release_count_ >= image_capacity_;
    if (!accepting_ || full) {
        return finish(!accepting_ ? Admission::kStopped : Admission::kBusy,
                      !accepting_ ? EventDecision::kStopped : EventDecision::kBusy);
    }
    ImageSlot& slot = image_slots_[image_tail_];
    slot.event = event;
    slot.event.decision = EventDecision::kAccepted;
    slot.event.image_index = next_image_index_++;
    (void)std::snprintf(slot.event.image_name.data(), slot.event.image_name.size(),
                        "img/frame_%08llu.%s",
                        static_cast<unsigned long long>(slot.event.image_index),
                        extension(config_.image_format));
    slot.lease = std::move(lease);
    slot.occupied = true;
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

bool Recorder::enqueue_event(const FrameEvent& event) noexcept {
    std::unique_lock<std::mutex> lock(event_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !event_running_ || event_count_ >= event_capacity_) {
        event_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    EventSlot& slot = event_slots_[event_tail_];
    slot.event = event;
    slot.occupied = true;
    event_tail_ = (event_tail_ + 1U) % event_capacity_;
    ++event_count_;
    lock.unlock();
    event_cv_.notify_one();
    return true;
}

std::uint32_t Recorder::drain_releases() noexcept {
    std::uint32_t drained = 0;
    for (;;) {
        camera::FrameLease lease{};
        {
            std::unique_lock<std::mutex> lock(image_mutex_, std::try_to_lock);
            if (!lock.owns_lock() || release_count_ == 0) break;
            ReleaseSlot& slot = release_slots_[release_head_];
            lease = std::move(slot.lease);
            slot.occupied = false;
            release_head_ = (release_head_ + 1U) % image_capacity_;
            --release_count_;
        }
        if (lease.valid()) {
            auto released = lease.release();
            if (!released) lease_release_fail_.fetch_add(1, std::memory_order_relaxed);
        }
        ++drained;
    }
    return drained;
}

std::uint32_t Recorder::pending_releases() const noexcept {
    std::lock_guard<std::mutex> lock(image_mutex_);
    return release_count_;
}

bool Recorder::copy_image(ImageSlot& slot, const camera::FrameView& view) noexcept {
    if (!valid_view(view)) return false;
    auto* destination = slot.pixels.get();
    for (std::uint32_t y = 0; y < view.height; ++y) {
        const auto* source = view.luma.data() + static_cast<std::size_t>(y) * view.stride;
        std::memcpy(destination + static_cast<std::size_t>(y) * view.width, source, view.width);
    }
    return true;
}

bool Recorder::write_pgm(const ImageSlot& slot, const char* path) const noexcept {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) return false;
    const bool header_ok = std::fprintf(file, "P5\n%u %u\n255\n", config_.width, config_.height) > 0;
    const std::size_t bytes = header_ok ? std::fwrite(slot.pixels.get(), 1, frame_bytes_, file) : 0;
    const int close_result = std::fclose(file);
    return header_ok && bytes == frame_bytes_ && close_result == 0;
}

bool Recorder::write_pbm(const ImageSlot& slot, const char* path) const noexcept {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) return false;
    bool ok = std::fprintf(file, "P4\n%u %u\n", config_.width, config_.height) > 0;
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(slot.pixels.get());
    auto* packed = reinterpret_cast<std::uint8_t*>(pbm_row_.get());
    for (std::uint32_t y = 0; ok && y < config_.height; ++y) {
        std::memset(packed, 0, pbm_row_bytes_);
        const auto* row = pixels + static_cast<std::size_t>(y) * config_.width;
        for (std::uint32_t x = 0; x < config_.width; ++x)
            if (row[x] < config_.pbm_threshold)
                packed[x / 8U] |= static_cast<std::uint8_t>(0x80U >> (x % 8U));
        ok = std::fwrite(packed, 1, pbm_row_bytes_, file) == pbm_row_bytes_;
    }
    const int close_result = std::fclose(file);
    return ok && close_result == 0;
}

bool Recorder::write_image(const ImageSlot& slot) const noexcept {
    char path[kMaxPath]{};
    const int n = std::snprintf(path, sizeof(path), "%s/%s", session_dir_, slot.event.image_name.data());
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(path)) return false;
    return config_.image_format == ImageFormat::kPbm ? write_pbm(slot, path) : write_pgm(slot, path);
}

void* Recorder::image_thread_entry(void* context) noexcept {
    static_cast<Recorder*>(context)->image_loop();
    return nullptr;
}

void* Recorder::event_thread_entry(void* context) noexcept {
    static_cast<Recorder*>(context)->event_loop();
    return nullptr;
}

void Recorder::image_loop() noexcept {
    for (;;) {
        ImageSlot* slot = nullptr;
        {
            std::unique_lock<std::mutex> lock(image_mutex_);
            image_cv_.wait(lock, [this] { return image_count_ != 0 || !accepting_; });
            if (image_count_ == 0 && !accepting_) break;
            slot = &image_slots_[image_head_];
        }
        const auto view = slot->lease.view();
        const bool copied = copy_image(*slot, view);
        if (copied) copied_.fetch_add(1, std::memory_order_relaxed);
        else copy_fail_.fetch_add(1, std::memory_order_relaxed);
        camera::FrameLease source_lease = std::move(slot->lease);
        if (copied) {
            if (write_image(*slot)) {
                written_.fetch_add(1, std::memory_order_relaxed);
                bytes_.fetch_add(config_.image_format == ImageFormat::kPbm
                                     ? pbm_row_bytes_ * static_cast<std::size_t>(config_.height)
                                     : frame_bytes_,
                                 std::memory_order_relaxed);
            } else {
                write_fail_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        {
            std::lock_guard<std::mutex> lock(image_mutex_);
            ReleaseSlot& release = release_slots_[release_tail_];
            release.lease = std::move(source_lease);
            release.occupied = true;
            release_tail_ = (release_tail_ + 1U) % image_capacity_;
            ++release_count_;
            slot->occupied = false;
            image_head_ = (image_head_ + 1U) % image_capacity_;
            --image_count_;
        }
        image_cv_.notify_all();
    }
}

void Recorder::maybe_flush_events(bool force) noexcept {
    if (event_file_ == nullptr) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_event_flush_ < config_.flush_period) return;
    (void)std::fflush(event_file_);
    last_event_flush_ = now;
}

void Recorder::event_loop() noexcept {
    for (;;) {
        FrameEvent event{};
        {
            std::unique_lock<std::mutex> lock(event_mutex_);
            event_cv_.wait(lock, [this] { return event_count_ != 0 || !event_running_; });
            if (event_count_ == 0 && !event_running_) break;
            EventSlot& slot = event_slots_[event_head_];
            event = slot.event;
            slot.occupied = false;
            event_head_ = (event_head_ + 1U) % event_capacity_;
            --event_count_;
        }
        if (event_file_ != nullptr) {
            const auto decision = decision_text(event.decision);
            std::fprintf(event_file_, "%llu,%llu,%llu,%u,%lld,%.*s,%llu,%s\n",
                         static_cast<unsigned long long>(event.input_index),
                         static_cast<unsigned long long>(event.source_sequence),
                         static_cast<unsigned long long>(event.source_pts),
                         event.source_pts_timebase_validated ? 1U : 0U,
                         static_cast<long long>(event.received_us),
                         static_cast<int>(decision.size()), decision.data(),
                         static_cast<unsigned long long>(event.image_index),
                         event.image_name.data());
        }
        if (config_.event_sink != nullptr) config_.event_sink(event, config_.event_sink_context);
        maybe_flush_events(false);
    }
    maybe_flush_events(true);
}

std::string_view Recorder::error_text(Error error) noexcept {
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

std::string_view Recorder::decision_text(EventDecision decision) noexcept {
    switch (decision) {
        case EventDecision::kAccepted: return "accepted";
        case EventDecision::kDownsampled: return "downsampled";
        case EventDecision::kBusy: return "busy";
        case EventDecision::kStopped: return "stopped";
        case EventDecision::kInvalidFrame: return "invalid_frame";
    }
    return "unknown";
}

Stats Recorder::stats() const noexcept {
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

}  // namespace dart::recorder

namespace dart::recording {
using Recorder = ::dart::recorder::Recorder;
using Config = ::dart::recorder::Config;
using Stats = ::dart::recorder::Stats;
using FrameEvent = ::dart::recorder::FrameEvent;
using FrameEventSink = ::dart::recorder::FrameEventSink;
using Admission = ::dart::recorder::Admission;
using EventDecision = ::dart::recorder::EventDecision;
using ImageFormat = ::dart::recorder::ImageFormat;
using Error = ::dart::recorder::Error;
}  // namespace dart::recording

#endif  // DART_RECORDING_RECORDER_CPP
