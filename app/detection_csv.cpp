#ifndef DART_APP_DETECTION_CSV_CPP
#define DART_APP_DETECTION_CSV_CPP

#include <cstdio>
#include <memory>
#include <new>

#include "camera/frame_view.cpp"
#include "directories.cpp"
#include "src/detector/result.cpp"

namespace dart::app {

class DetectionCsv {
public:
    bool open(const char* path) noexcept {
        ensure_parent(path);
        file_ = std::fopen(path, "w");
        if (file_ == nullptr)
            return false;
        // Keep all rows of a status interval in RAM. Small libc buffers were
        // forcing SD writes (observed >100 ms) inside the acquisition loop.
        buffer_.reset(new (std::nothrow) char[64 * 1024]);
        if (buffer_)
            (void)std::setvbuf(file_, buffer_.get(), _IOFBF, 64 * 1024);
        std::fprintf(
            file_, "sequence,received_us,source_pts,kind,valid,cx,cy,area,radius,"
                   "quality,clipped,missed_frames,candidates,full_frame,rescanned,"
                   "win_x0,win_y0,win_x1,win_y1,admission\n");
        std::fflush(file_);
        return true;
    }

    void
        row(const camera::FrameView& view, const detection::DetectResult* result,
            const char* admission) noexcept {
        if (file_ == nullptr)
            return;
        if (result == nullptr) {
            /* 识别关闭时仍然记录帧身份与放行决定，保证 CSV 覆盖每一帧。 */
            std::fprintf(
                file_, "%llu,%lld,%llu,none,0,,,,,,,,,,,%s\n",
                static_cast<unsigned long long>(view.sequence),
                static_cast<long long>(view.received.count()),
                static_cast<unsigned long long>(view.source_pts), admission);
            return;
        }
        const auto& target = result->target;
        const auto& scan = result->scan;
        std::fprintf(
            file_,
            "%llu,%lld,%llu,%s,%d,%.1f,%.1f,%u,%.2f,%.3f,%d,%u,%u,%d,%d,"
            "%u,%u,%u,%u,%s\n",
            static_cast<unsigned long long>(result->source.frame_id),
            static_cast<long long>(result->source.received.count()),
            static_cast<unsigned long long>(result->source.source_pts), kind_text(result->kind),
            target.valid ? 1 : 0, static_cast<double>(target.center.x),
            static_cast<double>(target.center.y), target.area, static_cast<double>(target.radius),
            static_cast<double>(target.quality), target.clipped ? 1 : 0, result->missed_frames,
            scan.candidates, scan.full_frame ? 1 : 0, scan.rescanned ? 1 : 0, scan.search_window.x,
            scan.search_window.y, scan.search_window.x + scan.search_window.width,
            scan.search_window.y + scan.search_window.height, admission);
    }

    void flush() noexcept {
        if (file_ == nullptr)
            return;
        std::fflush(file_);
    }

    void close() noexcept {
        if (file_ == nullptr)
            return;
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
        buffer_.reset();
    }

private:
    std::unique_ptr<char[]> buffer_{};
    std::FILE* file_ = nullptr;

private:
    static const char* kind_text(detection::TargetKind kind) noexcept {
        switch (kind) {
        case detection::TargetKind::kNone: return "none";
        case detection::TargetKind::kLight: return "light";
        case detection::TargetKind::kArmor: return "armor";
        }
        return "none";
    }
};

} // namespace dart::app

#endif // DART_APP_DETECTION_CSV_CPP
