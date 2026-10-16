#ifndef DART_RUNTIME_RECORDOR_CONFIG_CPP
#define DART_RUNTIME_RECORDOR_CONFIG_CPP

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace dart::recorder {

enum class EventDecision : std::uint8_t {
    kAccepted,
    kDownsampled,
    kBusy,
    kStopped,
    kInvalidFrame,
};

class FrameEvent {
public:
    std::uint64_t input_index = 0;
    std::uint64_t source_sequence = 0;
    std::uint64_t source_pts = 0;
    std::int64_t received_us = 0;
    bool source_pts_timebase_validated = false;
    std::uint64_t image_index = 0;
    EventDecision decision = EventDecision::kStopped;
    std::array<char, 48> image_name{};
};

using FrameEventSink = void (*)(const FrameEvent&, void*) noexcept;

enum class ImageFormat : std::uint8_t { kPgm, kPbm };

class Config {
public:
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /* Target images per second, sampled using the monotonic receipt timestamp.
     * Checked before queue availability; busy slots still drop without waiting. */
    std::uint32_t fps = 30;
    /* Hard image-slot cap; a slot stays busy through copy and file write. */
    std::uint32_t queue_slots = 2;
    /* Separate bounded metadata queue for one CSV row per enqueued submission; drops
     * are counted instead of blocking capture. */
    std::uint32_t event_slots = 512;
    ImageFormat image_format = ImageFormat::kPgm;
    std::uint8_t pbm_threshold = 128;
    std::chrono::milliseconds flush_period{1000};
    std::string_view output_directory = "records";
    std::uint32_t keep_sessions = 0; /* 0 = never remove earlier sessions */
    FrameEventSink event_sink = nullptr;
    void* event_sink_context = nullptr;
};

} // namespace dart::recorder

#endif // DART_RUNTIME_RECORDOR_CONFIG_CPP
