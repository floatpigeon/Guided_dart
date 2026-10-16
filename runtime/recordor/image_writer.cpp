#ifndef DART_RUNTIME_RECORDOR_IMAGE_WRITER_CPP
#define DART_RUNTIME_RECORDOR_IMAGE_WRITER_CPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <span>

#include "camera/frame_view.cpp"
#include "session.cpp"

namespace dart::recorder {

class ImageWriter final {
public:
    ImageWriter(const Config& config, const Session& session) noexcept
        : config_(config)
        , session_(session) {}
    bool prepare(std::size_t frame_bytes) noexcept {
        frame_bytes_ = frame_bytes;
        if (config_.image_format == ImageFormat::kPbm) {
            pbm_row_bytes_ = (static_cast<std::size_t>(config_.width) + 7U) / 8U;
            pbm_row_.reset(new (std::nothrow) std::byte[pbm_row_bytes_]);
            return pbm_row_ != nullptr;
        }
        return true;
    }
    bool valid_view(const camera::FrameView& view) const noexcept;
    bool copy_image(std::span<std::byte> pixels, const camera::FrameView& view) noexcept;
    bool write_image(std::span<const std::byte> pixels, const char* image_name) const noexcept;
    std::size_t payload_bytes() const noexcept {
        return config_.image_format == ImageFormat::kPbm
                 ? pbm_row_bytes_ * static_cast<std::size_t>(config_.height)
                 : frame_bytes_;
    }

private:
    static constexpr std::size_t kMaxPath = RecorderError::kMaxPath;
    bool write_pgm(std::span<const std::byte>, const char*) const noexcept;
    bool write_pbm(std::span<const std::byte>, const char*) const noexcept;
    const Config& config_;
    const Session& session_;
    std::size_t frame_bytes_ = 0;
    std::size_t pbm_row_bytes_ = 0;
    std::unique_ptr<std::byte[]> pbm_row_{};
};

inline bool ImageWriter::valid_view(const camera::FrameView& view) const noexcept {
    if (view.width != config_.width || view.height != config_.height || view.stride < view.width
        || view.luma.data() == nullptr)
        return false;
    const std::uint64_t required =
        static_cast<std::uint64_t>(view.height - 1U) * view.stride + view.width;
    return required <= view.luma.size();
}

inline bool
    ImageWriter::copy_image(std::span<std::byte> pixels, const camera::FrameView& view) noexcept {
    if (!valid_view(view) || pixels.size() < frame_bytes_)
        return false;
    auto* destination = pixels.data();
    for (std::uint32_t y = 0; y < view.height; ++y) {
        const auto* source = view.luma.data() + static_cast<std::size_t>(y) * view.stride;
        std::memcpy(destination + static_cast<std::size_t>(y) * view.width, source, view.width);
    }
    return true;
}

inline bool ImageWriter::write_pgm(std::span<const std::byte> pixels, const char* path) const noexcept {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
        return false;
    const bool header_ok =
        std::fprintf(file, "P5\n%u %u\n255\n", config_.width, config_.height) > 0;
    const std::size_t bytes = header_ok ? std::fwrite(pixels.data(), 1, frame_bytes_, file) : 0;
    const int close_result = std::fclose(file);
    return header_ok && bytes == frame_bytes_ && close_result == 0;
}

inline bool ImageWriter::write_pbm(std::span<const std::byte> pixels, const char* path) const noexcept {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
        return false;
    bool ok = std::fprintf(file, "P4\n%u %u\n", config_.width, config_.height) > 0;
    const auto* gray = reinterpret_cast<const std::uint8_t*>(pixels.data());
    auto* packed = reinterpret_cast<std::uint8_t*>(pbm_row_.get());
    for (std::uint32_t y = 0; ok && y < config_.height; ++y) {
        std::memset(packed, 0, pbm_row_bytes_);
        const auto* row = gray + static_cast<std::size_t>(y) * config_.width;
        for (std::uint32_t x = 0; x < config_.width; ++x)
            if (row[x] < config_.pbm_threshold)
                packed[x / 8U] |= static_cast<std::uint8_t>(0x80U >> (x % 8U));
        ok = std::fwrite(packed, 1, pbm_row_bytes_, file) == pbm_row_bytes_;
    }
    const int close_result = std::fclose(file);
    return ok && close_result == 0;
}

inline bool ImageWriter::write_image(
    std::span<const std::byte> pixels, const char* image_name) const noexcept {
    if (pixels.size() < frame_bytes_)
        return false;
    char path[kMaxPath]{};
    const int n = std::snprintf(path, sizeof(path), "%s/%s", session_.directory(), image_name);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(path))
        return false;
    return config_.image_format == ImageFormat::kPbm ? write_pbm(pixels, path)
                                                   : write_pgm(pixels, path);
}

} // namespace dart::recorder

#endif // DART_RUNTIME_RECORDOR_IMAGE_WRITER_CPP
