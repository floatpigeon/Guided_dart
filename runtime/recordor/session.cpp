#ifndef DART_RUNTIME_RECORDOR_SESSION_CPP
#define DART_RUNTIME_RECORDOR_SESSION_CPP

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.cpp"
#include "error.cpp"

namespace dart::recorder {

class Session final {
public:
    static constexpr std::size_t kMaxPath = RecorderError::kMaxPath;
    Session(const Config& config, RecorderError& error) noexcept
        : config_(config)
        , error_(error) {}
    bool open() noexcept;
    const char* directory() const noexcept { return session_dir_; }
    const char* event_path() const noexcept { return event_path_; }

private:
    bool scan_sessions(const char*, std::uint32_t&, std::uint32_t&, std::uint32_t&) noexcept;
    bool remove_session(const char*, std::uint32_t) noexcept;
    void set_error(Error error) noexcept { error_.set_error(error); }
    void set_file_error(Error error, const char* path) noexcept {
        error_.set_file_error(error, path);
    }
    const Config& config_;
    RecorderError& error_;
    char session_dir_[kMaxPath]{};
    char image_dir_[kMaxPath]{};
    char event_path_[kMaxPath]{};

private:
    static bool mkdir_if_needed(const char* path) noexcept {
        if (::mkdir(path, 0777) == 0)
            return true;
        if (errno != EEXIST)
            return false;
        struct stat st{};
        return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    }

    static std::uint32_t session_number(const char* name) noexcept {
        if (std::strlen(name) != 8 || std::memcmp(name, "rec_", 4) != 0)
            return 0;
        std::uint32_t number = 0;
        for (int i = 4; i < 8; ++i) {
            if (name[i] < '0' || name[i] > '9')
                return 0;
            number = number * 10U + static_cast<std::uint32_t>(name[i] - '0');
        }
        return number;
    }

    static bool recorder_file_name(const char* name, bool image) noexcept {
        if (!image)
            return std::strcmp(name, "frames.csv") == 0;
        if (std::strlen(name) != 18 || std::memcmp(name, "frame_", 6) != 0)
            return false;
        for (int i = 6; i < 14; ++i)
            if (name[i] < '0' || name[i] > '9')
                return false;
        return std::strcmp(name + 14, ".pgm") == 0 || std::strcmp(name + 14, ".pbm") == 0;
    }
};

inline bool Session::scan_sessions(
    const char* root, std::uint32_t& count, std::uint32_t& oldest, std::uint32_t& newest) noexcept {
    DIR* dir = ::opendir(root);
    if (dir == nullptr) {
        set_file_error(Error::kDirectory, root);
        return false;
    }
    count = oldest = newest = 0;
    for (;;) {
        errno = 0;
        const dirent* entry = ::readdir(dir);
        if (entry == nullptr) {
            if (errno != 0) {
                set_file_error(Error::kDirectory, root);
                ::closedir(dir);
                return false;
            }
            break;
        }
        const std::uint32_t number = session_number(entry->d_name);
        if (number == 0)
            continue;
        char path[kMaxPath]{};
        const int n = std::snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(path)) {
            set_error(Error::kPathTooLong);
            ::closedir(dir);
            return false;
        }
        struct stat st{};
        if (::lstat(path, &st) != 0) {
            set_file_error(Error::kDirectory, path);
            ::closedir(dir);
            return false;
        }
        if (!S_ISDIR(st.st_mode))
            continue;
        ++count;
        if (oldest == 0 || number < oldest)
            oldest = number;
        if (number > newest)
            newest = number;
    }
    ::closedir(dir);
    return true;
}

inline bool Session::remove_session(const char* root, std::uint32_t index) noexcept {
    char path[kMaxPath]{};
    const int n = std::snprintf(path, sizeof(path), "%s/rec_%04u", root, index);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(path)) {
        set_error(Error::kPathTooLong);
        return false;
    }
    DIR* session = ::opendir(path);
    if (session == nullptr) {
        set_file_error(Error::kDirectory, path);
        return false;
    }
    for (;;) {
        errno = 0;
        const dirent* entry = ::readdir(session);
        if (entry == nullptr) {
            if (errno != 0) {
                set_file_error(Error::kDirectory, path);
                ::closedir(session);
                return false;
            }
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
            continue;
        if (std::strcmp(entry->d_name, "img") != 0 && std::strcmp(entry->d_name, "logs") != 0) {
            errno = EPERM;
            set_file_error(Error::kDirectory, path);
            ::closedir(session);
            return false;
        }
    }
    ::closedir(session);
    // Validate both subdirectories before deleting anything. Unknown files are never removed.
    for (int pass = 0; pass < 2; ++pass) {
        for (int kind = 0; kind < 2; ++kind) {
            const bool image = kind == 0;
            char subdir[kMaxPath]{};
            const int sub_n =
                std::snprintf(subdir, sizeof(subdir), "%s/%s", path, image ? "img" : "logs");
            if (sub_n <= 0 || static_cast<std::size_t>(sub_n) >= sizeof(subdir)) {
                set_error(Error::kPathTooLong);
                return false;
            }
            DIR* dir = ::opendir(subdir);
            if (dir == nullptr) {
                if (errno == ENOENT)
                    continue; // A reset may leave an incomplete session.
                set_file_error(Error::kDirectory, subdir);
                return false;
            }
            struct stat dir_st{};
            if (::lstat(subdir, &dir_st) != 0) {
                set_file_error(Error::kDirectory, subdir);
                ::closedir(dir);
                return false;
            }
            if (!S_ISDIR(dir_st.st_mode)) {
                errno = EPERM;
                set_file_error(Error::kDirectory, subdir);
                ::closedir(dir);
                return false;
            }
            for (;;) {
                errno = 0;
                const dirent* entry = ::readdir(dir);
                if (entry == nullptr) {
                    if (errno != 0) {
                        set_file_error(Error::kDirectory, subdir);
                        ::closedir(dir);
                        return false;
                    }
                    break;
                }
                if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
                    continue;
                char file[kMaxPath]{};
                const int file_n =
                    std::snprintf(file, sizeof(file), "%s/%s", subdir, entry->d_name);
                if (file_n <= 0 || static_cast<std::size_t>(file_n) >= sizeof(file)) {
                    set_error(Error::kPathTooLong);
                    ::closedir(dir);
                    return false;
                }
                if (!recorder_file_name(entry->d_name, image)) {
                    errno = EPERM;
                    set_file_error(Error::kDirectory, file);
                    ::closedir(dir);
                    return false;
                }
                struct stat st{};
                if (::lstat(file, &st) != 0) {
                    set_file_error(Error::kDirectory, file);
                    ::closedir(dir);
                    return false;
                }
                if (!S_ISREG(st.st_mode)) {
                    errno = EPERM;
                    set_file_error(Error::kDirectory, file);
                    ::closedir(dir);
                    return false;
                }
                if (pass == 1 && ::unlink(file) != 0) {
                    set_file_error(Error::kDirectory, file);
                    ::closedir(dir);
                    return false;
                }
            }
            ::closedir(dir);
            if (pass == 1 && ::rmdir(subdir) != 0) {
                set_file_error(Error::kDirectory, subdir);
                return false;
            }
        }
    }
    if (::rmdir(path) != 0) {
        set_file_error(Error::kDirectory, path);
        return false;
    }
    return true;
}

inline bool Session::open() noexcept {
    const auto root = config_.output_directory;
    char root_path[kMaxPath]{};
    std::memcpy(root_path, root.data(), root.size());
    root_path[root.size()] = '\0';
    if (!mkdir_if_needed(root_path)) {
        set_file_error(Error::kDirectory, root_path);
        return false;
    }
    std::uint32_t count = 0, oldest = 0, newest = 0;
    if (!scan_sessions(root_path, count, oldest, newest))
        return false;
    while (config_.keep_sessions > 0 && count >= config_.keep_sessions) {
        if (!remove_session(root_path, oldest) || !scan_sessions(root_path, count, oldest, newest))
            return false;
    }
    bool created = false;
    for (std::uint32_t offset = 0; offset < 9999; ++offset) {
        const std::uint32_t i = (newest + offset) % 9999U + 1U;
        const int n = std::snprintf(
            session_dir_, sizeof(session_dir_), "%.*s/rec_%04u", static_cast<int>(root.size()),
            root.data(), i);
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(session_dir_)) {
            set_error(Error::kPathTooLong);
            return false;
        }
        if (::mkdir(session_dir_, 0777) != 0) {
            if (errno == EEXIST)
                continue;
            set_file_error(Error::kDirectory, session_dir_);
            return false;
        }
        created = true;
        break;
    }
    if (!created) {
        errno = ENOSPC;
        set_file_error(Error::kDirectory, root_path);
        return false;
    }

    char log_dir[kMaxPath]{};
    const int image_n = std::snprintf(image_dir_, sizeof(image_dir_), "%s/img", session_dir_);
    const int log_n = std::snprintf(log_dir, sizeof(log_dir), "%s/logs", session_dir_);
    const int event_n = std::snprintf(event_path_, sizeof(event_path_), "%s/frames.csv", log_dir);
    if (image_n <= 0 || log_n <= 0 || event_n <= 0
        || static_cast<std::size_t>(image_n) >= sizeof(image_dir_)
        || static_cast<std::size_t>(log_n) >= sizeof(log_dir)
        || static_cast<std::size_t>(event_n) >= sizeof(event_path_)) {
        set_error(Error::kPathTooLong);
        return false;
    }
    if (!mkdir_if_needed(log_dir)) {
        set_file_error(Error::kDirectory, log_dir);
        return false;
    }
    if (!mkdir_if_needed(image_dir_)) {
        set_file_error(Error::kDirectory, image_dir_);
        return false;
    }
    return true;
}

} // namespace dart::recorder

#endif // DART_RUNTIME_RECORDOR_SESSION_CPP
