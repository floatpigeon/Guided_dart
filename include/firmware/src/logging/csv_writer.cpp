#ifndef DART_LOGGING_CSV_WRITER_CPP
#define DART_LOGGING_CSV_WRITER_CPP

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <new>
#include <unistd.h>

#include "storage/directories.cpp"

namespace dart::logging {

// Own buffering and file durability; the caller owns column and row semantics.
class CsvWriter final {
public:
    CsvWriter() = default;
    ~CsvWriter() { close(); }
    CsvWriter(const CsvWriter&) = delete;
    CsvWriter& operator=(const CsvWriter&) = delete;

    bool open(const char* path, const char* header) noexcept {
        close();
        storage::ensure_parent(path);
        file_ = std::fopen(path, "w");
        if (file_ == nullptr)
            return false;
        buffer_.reset(new (std::nothrow) char[kBufferBytes]);
        if (buffer_)
            (void)std::setvbuf(file_, buffer_.get(), _IOFBF, kBufferBytes);
        (void)std::fputs(header, file_);
        flush();
        return true;
    }
    void row(const char* format, ...) noexcept {
        if (file_ == nullptr)
            return;
        va_list args;
        va_start(args, format);
        (void)std::vfprintf(file_, format, args);
        va_end(args);
    }
    void flush() noexcept {
        if (file_ != nullptr) {
            (void)std::fflush(file_);
            (void)::fsync(::fileno(file_));
        }
    }
    void close() noexcept {
        if (file_ != nullptr) {
            flush();
            (void)std::fclose(file_);
            file_ = nullptr;
        }
        buffer_.reset();
    }

private:
    static constexpr std::size_t kBufferBytes = 64U * 1024U;
    std::unique_ptr<char[]> buffer_{};
    std::FILE* file_ = nullptr;
};

} // namespace dart::logging

#endif // DART_LOGGING_CSV_WRITER_CPP
