/*
 * dart_app —— 板端业务应用的组件组装与生命周期编排
 *
 * main.cpp 只提供进程入口；本文件负责配置解析、组件创建、运行调度与退出。
 * 当前接入相机、识别和录像；后续 IMU、控制等任务也在这里组装，具体设备操作
 * 和算法留在各自模块中，各任务按自己的时序运行。
 *
 * 为什么必须是单进程：相机 ACL 在 create() 里自建并独占 dart_media（全局 VB
 * 初始化 + VICAP/VB 池），同一时刻只允许一个消费者。v1 实测把同一程序在清单里
 * 放多条，5 个实例几乎同时抢 VB/VICAP，只有 9/28 次写出日志，其余撞
 * FATAL: vb_set_config 静默死掉。因此清单里只允许出现一条本程序。
 *
 * 流水线（单线程采集，录像在 Recorder 内部线程拷贝/写盘）：
 *   acquire() -> FrameLease
 *     -> detect(lease.view())        识别同步借用像素，不取帧、不释放租约
 *     -> recorder.try_submit(lease)  被接收则所有权转给 Recorder；否则本线程释放
 *     -> recorder.drain_releases()   每拍调用：录像线程归还的租约在采集线程释放，
 *                                    保证相机 ACL 的调用始终串行
 *
 * 退出契约（DEBUG_GUIDE §8，每一步前后各一条日志）：
 *   主循环出界/SIGINT/SIGTERM -> recorder.stop() -> 排空 release 队列
 *   -> camera.stop() -> camera 析构（deinit/destroy/media destroy）-> 摘要
 * 停止输入必须发生在 camera.stop() 之前；recorder 仍持租约时 stop 会返回 BUSY。
 *
 * 构建：bash .script/build-rtos dart_app  ->  build-cross/dart_app/dart_app
 * 板端（先手动验证，再交给 launcher）：
 *   msh> /sdcard/app/dart_app --seconds 10 --record on
 */
#include "application.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <memory>
#include <new>
#include <signal.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#include "camera/camera.hpp"
#include "camera/k230/camera.hpp"
#include "src/detector/detector.hpp"
/* 录像模块把声明与实现合并在一个带包含保护的翻译单元里（没有 recorder.hpp），
 * 因此本应用采用 include 消费方式，链接时不要再加 dart_recording（会重复定义）。 */
#include "runtime/recordor/recorder.cpp"

namespace dart::app {
namespace {

/* 本文件在匿名命名空间里工作，模块名一律用短别名；局部变量不得与别名同名
 * （因此相机句柄叫 cam、录像器叫 rec）。 */
namespace camera = dart::camera;
namespace detection = dart::detection;
namespace recorder = dart::recorder;

/* 租约容量是编译期参数：每个在途租约占用一个驱动通道缓冲，因此必须
 * kLeaseCapacity <= channel_buffers（相机 ACL 会在 create 时校验这对关系）。
 * 取 4 是为了让"识别中/录像拷贝中/写盘中"的帧彼此不互锁。 */
constexpr std::uint32_t kLeaseCapacity = 4;
constexpr std::uint32_t kChannelBuffers = 6;
constexpr std::uint32_t kCaptureBuffers = 4;

constexpr std::string_view kDefaultLog = "/sdcard/app/logs/dart_app.log";
constexpr std::string_view kDefaultRecordDir = "/sdcard/app/records";

volatile sig_atomic_t g_stop = 0;

extern "C" void on_signal(int /*signum*/)
{
    /* 信号处理器只做异步信号安全的最小写入：置标志。 */
    g_stop = 1;
}

std::int64_t mono_ms() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void sleep_ms(int ms) noexcept
{
    if (ms <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* Structured logs close after each write so MTP can read the current file size.
 * Status and timing lines share one sync; errors still sync immediately. */
class Logger {
public:
    bool open(std::string_view path) noexcept
    {
        if (path.empty() || path.size() >= sizeof(path_))
            return false;
        std::memcpy(path_, path.data(), path.size());
        path_[path.size()] = '\0';
        const int fd = ::open(path_, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0)
            return false;
        ::close(fd);
        return true;
    }

    void begin_batch() noexcept { batching_ = true; }

    void end_batch() noexcept
    {
        batching_ = false;
        if (fd_ >= 0) {
            ::fsync(fd_);
            ::close(fd_);
            fd_ = -1;
        }
    }

    void line(const char* fmt, ...) noexcept
    {
        char body[512];
        char out[640];
        va_list ap;
        va_start(ap, fmt);
        const int body_n = std::vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);
        if (body_n < 0)
            return;
        int out_n = std::snprintf(out, sizeof(out), "ts_ms=%lld %s",
                                  static_cast<long long>(mono_ms()), body);
        if (out_n < 0)
            return;
        if (static_cast<std::size_t>(out_n) >= sizeof(out))
            out_n = static_cast<int>(sizeof(out)) - 1;
        if (fd_ < 0 && path_[0] != '\0')
            fd_ = ::open(path_, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd_ >= 0) {
            (void)!::write(fd_, out, static_cast<std::size_t>(out_n));
            if (!batching_)
                end_batch();
        } else {
            (void)!::write(STDERR_FILENO, out, static_cast<std::size_t>(out_n));
        }
    }

private:
    char path_[512]{};
    int fd_ = -1;
    bool batching_ = false;
};

std::string_view operation_text(camera::Operation operation) noexcept
{
    switch (operation) {
        case camera::Operation::kUnknown: return "unknown";
        case camera::Operation::kConfigValidate: return "config_validate";
        case camera::Operation::kMediaCreate: return "media_create";
        case camera::Operation::kMediaDestroy: return "media_destroy";
        case camera::Operation::kCreate: return "create";
        case camera::Operation::kInitialize: return "initialize";
        case camera::Operation::kStart: return "start";
        case camera::Operation::kAcquire: return "acquire";
        case camera::Operation::kRelease: return "release";
        case camera::Operation::kStop: return "stop";
        case camera::Operation::kDeinitialize: return "deinitialize";
        case camera::Operation::kDestroy: return "destroy";
        case camera::Operation::kGetState: return "get_state";
        case camera::Operation::kGetCaps: return "get_caps";
        case camera::Operation::kExposureRange: return "exposure_range";
        case camera::Operation::kSetExposure: return "set_exposure";
    }
    return "unknown";
}

void log_camera_error(Logger& log, const char* op, const char* action,
                      const camera::CameraError& error) noexcept
{
    const auto& context = error.context();
    log.line("level=ERROR module=app op=%s code=%.*s native=%d detail=\"%.*s\" "
             "ctx_op=%.*s requested=%u observed=%u state=%u req_state=%u action=%s\n",
             op,
             static_cast<int>(error.category().size()), error.category().data(),
             error.code(),
             static_cast<int>(error.message().size()), error.message().data(),
             static_cast<int>(operation_text(context.operation).size()),
             operation_text(context.operation).data(),
             context.requested, context.observed, context.state,
             context.required_state, action);
}

std::int64_t parse_int(std::string_view text) noexcept
{
    char buf[32];
    char* end = nullptr;
    long value;
    if (text.empty() || text.size() >= sizeof(buf))
        return -1;
    std::memcpy(buf, text.data(), text.size());
    buf[text.size()] = '\0';
    errno = 0;
    value = std::strtol(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0' || value < 0)
        return -1;
    return static_cast<std::int64_t>(value);
}

/* 解析 "1920x1080"（也接受大写 X）。 */
bool parse_size(std::string_view text, std::uint32_t& width,
                std::uint32_t& height) noexcept
{
    const auto pos = text.find_first_of("xX");
    if (pos == std::string_view::npos)
        return false;
    const auto w = parse_int(text.substr(0, pos));
    const auto h = parse_int(text.substr(pos + 1));
    if (w <= 0 || h <= 0)
        return false;
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    return true;
}

bool parse_on_off(std::string_view text, bool& out) noexcept
{
    if (text == "on" || text == "1" || text == "true") {
        out = true;
        return true;
    }
    if (text == "off" || text == "0" || text == "false") {
        out = false;
        return true;
    }
    return false;
}

bool copy_path(char* out, std::size_t cap, std::string_view text) noexcept
{
    if (text.size() >= cap)
        return false;
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return true;
}

struct Args {
    std::uint32_t seconds = 0; /* 0 = 跑到信号 */
    std::uint32_t sensor_width = 1920;
    std::uint32_t sensor_height = 1080;
    std::uint32_t sensor_fps = 30;
    std::uint32_t output_width = 0; /* 0 = 跟随 sensor */
    std::uint32_t output_height = 0;
    std::uint32_t csi_port = 0;
    std::uint32_t mirror = 0;
    std::uint32_t exposure_us = 250;
    bool auto_exposure = false;
    bool vision_on = true;
    bool record_on = true;
    std::uint32_t record_fps = recorder::Config{}.fps;
    std::uint32_t record_keep = 0;
    bool pbm = false;
    char rec_dir[512] = "/sdcard/app/records";
    char det_csv[512] = ""; /* 空 = 关闭 */
    char log_path[512] = "/sdcard/app/logs/dart_app.log";
    std::uint32_t status_ms = 1000;
    std::uint32_t acquire_ms = 200;
    std::uint32_t max_acquire_errors = 8;
    std::uint32_t threshold = 50;
};

void usage(const char* argv0)
{
    std::printf("用法: %s [选项]\n\n", argv0);
    std::printf("  --seconds N      有界运行秒数，0 = 跑到 SIGINT/SIGTERM（默认 0）\n");
    std::printf("  --acq WxH        传感器模式（默认 1920x1080，需精确命中，无静默回退）\n");
    std::printf("  --fps N          传感器帧率（默认 30）\n");
    std::printf("  --out WxH        输出尺寸（默认跟随 --acq；宽 32 对齐、高偶数、不上采样）\n");
    std::printf("  --csi N          CSI 端口 0..2（默认 0）\n");
    std::printf("  --mirror N       0 无 / 1 水平 / 2 垂直 / 3 双向（默认 0）\n");
    std::printf("  --exp US         固定曝光微秒（默认 250；0 = 未定，会阻塞取帧）\n");
    std::printf("  --ae             自动曝光（与 --exp 互斥）\n");
    std::printf("  --vision on|off  识别开关（默认 on）\n");
    std::printf("  --record on|off  录像开关（默认 on）\n");
    std::printf("  --rec-fps N      目标录像帧率，按时间采样（默认 %u FPS）\n",
                recorder::Config{}.fps);
    std::printf("  --rec-keep N     只保留最近 N 轮录像，0 = 不清理（默认 0）\n");
    std::printf("  --rec-format F   pgm（灰度，默认）| pbm（按阈值二值化）\n");
    std::printf("  --rec-dir DIR    录像根目录（默认 %.*s）\n",
                static_cast<int>(kDefaultRecordDir.size()), kDefaultRecordDir.data());
    std::printf("  --det-csv PATH   识别结果 CSV；off 关闭（默认关闭）\n");
    std::printf("  --threshold N    二值化阈值（默认 50，UNVALIDATED）\n");
    std::printf("  --log PATH       结构化日志（默认 %.*s）\n",
                static_cast<int>(kDefaultLog.size()), kDefaultLog.data());
    std::printf("  --status-ms N    状态行间隔（默认 1000）\n");
    std::printf("  --acquire-ms N   单次取帧超时（默认 200）\n");
    std::printf("  --max-acquire-errors N  连续取帧错误上限，超过即退出（默认 8）\n");
    std::printf("  -h, --help       显示本帮助\n");
}

/* 返回 std::nullopt 表示参数非法（错误信息已打印）。 */
std::expected<Args, int> parse_args(int argc, char* argv[])
{
    Args args;
    args.det_csv[0] = '\0';

    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const auto need_value = [&](std::string_view& value) {
            if (i + 1 >= argc)
                return false;
            value = std::string_view{argv[++i]};
            return true;
        };

        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return std::unexpected<int>(0);
        }
        std::string_view value;
        if (a == "--seconds" || a == "--fps" || a == "--csi" || a == "--mirror"
            || a == "--exp" || a == "--rec-fps" || a == "--rec-keep" || a == "--threshold"
            || a == "--status-ms" || a == "--acquire-ms"
            || a == "--max-acquire-errors") {
            if (!need_value(value)) {
                std::fprintf(stderr, "%s: %.*s 缺少参数\n", argv[0],
                             static_cast<int>(a.size()), a.data());
                return std::unexpected<int>(2);
            }
            const auto n = parse_int(value);
            if (n < 0) {
                std::fprintf(stderr, "%s: %.*s 需要非负整数，得到 %.*s\n", argv[0],
                             static_cast<int>(a.size()), a.data(),
                             static_cast<int>(value.size()), value.data());
                return std::unexpected<int>(2);
            }
            if (a == "--seconds") args.seconds = static_cast<std::uint32_t>(n);
            else if (a == "--fps") args.sensor_fps = static_cast<std::uint32_t>(n);
            else if (a == "--csi") args.csi_port = static_cast<std::uint32_t>(n);
            else if (a == "--mirror") args.mirror = static_cast<std::uint32_t>(n);
            else if (a == "--exp") args.exposure_us = static_cast<std::uint32_t>(n);
            else if (a == "--rec-fps") {
                if (n == 0 || n > 1000000) {
                    std::fprintf(stderr, "%s: --rec-fps 必须在 1..1000000\n", argv[0]);
                    return std::unexpected<int>(2);
                }
                args.record_fps = static_cast<std::uint32_t>(n);
            }
            else if (a == "--rec-keep") args.record_keep = static_cast<std::uint32_t>(n);
            else if (a == "--threshold") args.threshold = static_cast<std::uint32_t>(n);
            else if (a == "--status-ms") args.status_ms = static_cast<std::uint32_t>(n);
            else if (a == "--acquire-ms") args.acquire_ms = static_cast<std::uint32_t>(n);
            else args.max_acquire_errors = static_cast<std::uint32_t>(n);
            continue;
        }
        if (a == "--acq" || a == "--out") {
            if (!need_value(value)) {
                std::fprintf(stderr, "%s: %.*s 缺少参数\n", argv[0],
                             static_cast<int>(a.size()), a.data());
                return std::unexpected<int>(2);
            }
            std::uint32_t w = 0;
            std::uint32_t h = 0;
            if (!parse_size(value, w, h)) {
                std::fprintf(stderr, "%s: %.*s 需要 WxH，得到 %.*s\n", argv[0],
                             static_cast<int>(a.size()), a.data(),
                             static_cast<int>(value.size()), value.data());
                return std::unexpected<int>(2);
            }
            if (a == "--acq") {
                args.sensor_width = w;
                args.sensor_height = h;
            } else {
                args.output_width = w;
                args.output_height = h;
            }
            continue;
        }
        if (a == "--vision" || a == "--record") {
            if (!need_value(value)) {
                std::fprintf(stderr, "%s: %.*s 缺少参数\n", argv[0],
                             static_cast<int>(a.size()), a.data());
                return std::unexpected<int>(2);
            }
            bool flag = false;
            if (!parse_on_off(value, flag)) {
                std::fprintf(stderr, "%s: %.*s 需要 on|off\n", argv[0],
                             static_cast<int>(a.size()), a.data());
                return std::unexpected<int>(2);
            }
            if (a == "--vision") args.vision_on = flag;
            else args.record_on = flag;
            continue;
        }
        if (a == "--ae") {
            args.auto_exposure = true;
            continue;
        }
        if (a == "--rec-format") {
            if (!need_value(value)) {
                std::fprintf(stderr, "%s: --rec-format 缺少参数\n", argv[0]);
                return std::unexpected<int>(2);
            }
            if (value == "pgm") args.pbm = false;
            else if (value == "pbm") args.pbm = true;
            else {
                std::fprintf(stderr, "%s: --rec-format 需要 pgm|pbm\n", argv[0]);
                return std::unexpected<int>(2);
            }
            continue;
        }
        if (a == "--rec-dir" || a == "--det-csv" || a == "--log") {
            if (!need_value(value)) {
                std::fprintf(stderr, "%s: %.*s 缺少参数\n", argv[0],
                             static_cast<int>(a.size()), a.data());
                return std::unexpected<int>(2);
            }
            if (a == "--rec-dir") {
                if (!copy_path(args.rec_dir, sizeof(args.rec_dir), value)) {
                    std::fprintf(stderr, "%s: --rec-dir 路径过长\n", argv[0]);
                    return std::unexpected<int>(2);
                }
            } else if (a == "--det-csv") {
                if (value == "off") {
                    args.det_csv[0] = '\0';
                } else if (!copy_path(args.det_csv, sizeof(args.det_csv), value)) {
                    std::fprintf(stderr, "%s: --det-csv 路径过长\n", argv[0]);
                    return std::unexpected<int>(2);
                }
            } else if (!copy_path(args.log_path, sizeof(args.log_path), value)) {
                std::fprintf(stderr, "%s: --log 路径过长\n", argv[0]);
                return std::unexpected<int>(2);
            }
            continue;
        }
        std::fprintf(stderr, "%s: 未知选项 %.*s（-h 查看用法）\n", argv[0],
                     static_cast<int>(a.size()), a.data());
        return std::unexpected<int>(2);
    }

    if (args.output_width == 0 || args.output_height == 0) {
        args.output_width = args.sensor_width;
        args.output_height = args.sensor_height;
    }
    if (args.auto_exposure && args.exposure_us != 0) {
        std::fprintf(stderr, "%s: --ae 与 --exp 互斥（--ae 时 initial_exposure_us 必须为 0）\n",
                     argv[0]);
        return std::unexpected<int>(2);
    }
    if (args.record_keep > 9999) {
        std::fprintf(stderr, "%s: --rec-keep 必须在 0..9999\n", argv[0]);
        return std::unexpected<int>(2);
    }
    if (args.status_ms == 0)
        args.status_ms = 1000;
    if (args.acquire_ms == 0)
        args.acquire_ms = 200;
    if (args.threshold > 255) {
        std::fprintf(stderr, "%s: --threshold 必须在 0..255\n", argv[0]);
        return std::unexpected<int>(2);
    }
    return args;
}

/* 尽力创建目录（单层）；父目录不存在也算成功路径由调用方兜底。 */
void ensure_dir(const char* dir) noexcept
{
    if (dir == nullptr || dir[0] == '\0')
        return;
    if (::mkdir(dir, 0777) == 0 || errno == EEXIST)
        return;
    if (errno == ENOENT) {
        char parent[512];
        const char* slash = std::strrchr(dir, '/');
        if (slash != nullptr && slash != dir) {
            const auto n = static_cast<std::size_t>(slash - dir);
            if (n < sizeof(parent)) {
                std::memcpy(parent, dir, n);
                parent[n] = '\0';
                (void)::mkdir(parent, 0777);
                (void)::mkdir(dir, 0777);
            }
        }
    }
}

void ensure_parent(const char* path) noexcept
{
    char parent[512];
    const char* slash;
    if (path == nullptr)
        return;
    slash = std::strrchr(path, '/');
    if (slash == nullptr || slash == path)
        return;
    const auto n = static_cast<std::size_t>(slash - path);
    if (n >= sizeof(parent))
        return;
    std::memcpy(parent, path, n);
    parent[n] = '\0';
    ensure_dir(parent);
}

const char* kind_text(detection::TargetKind kind) noexcept
{
    switch (kind) {
        case detection::TargetKind::kNone: return "none";
        case detection::TargetKind::kLight: return "light";
        case detection::TargetKind::kArmor: return "armor";
    }
    return "none";
}

const char* admission_text(recorder::Admission admission) noexcept
{
    switch (admission) {
        case recorder::Admission::kAccepted: return "accepted";
        case recorder::Admission::kDownsampled: return "downsampled";
        case recorder::Admission::kBusy: return "busy";
        case recorder::Admission::kStopped: return "stopped";
        case recorder::Admission::kInvalidFrame: return "invalid_frame";
    }
    return "unknown";
}

struct Counters {
    std::uint64_t frames = 0;
    std::uint64_t no_frame = 0;
    std::uint64_t lease_exhausted = 0;
    std::uint64_t acquire_errors = 0;
    std::uint64_t detect_calls = 0;
    std::uint64_t detect_errors = 0;
    std::uint64_t target_hits = 0;
    std::uint64_t release_ok = 0;
    std::uint64_t release_errors = 0;
    std::uint64_t rec_accepted = 0;
    std::uint64_t rec_downsampled = 0;
    std::uint64_t rec_busy = 0;
    std::uint64_t rec_invalid = 0;
};

// Windowed timings separate waiting for a frame from CPU and file-system work.
struct StageTiming {
    std::uint64_t total_us = 0;
    std::uint64_t max_us = 0;
    std::uint64_t samples = 0;

    void observe(std::chrono::steady_clock::time_point begin) noexcept
    {
        const auto us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - begin).count());
        total_us += us;
        max_us = std::max(max_us, us);
        ++samples;
    }

    double average_us() const noexcept
    {
        return samples > 0 ? static_cast<double>(total_us) / static_cast<double>(samples) : 0.0;
    }
};

class CsvWriter {
public:
    bool open(const char* path) noexcept
    {
        ensure_parent(path);
        file_ = std::fopen(path, "w");
        if (file_ == nullptr)
            return false;
        // Keep all rows of a status interval in RAM. Small libc buffers were
        // forcing SD writes (observed >100 ms) inside the acquisition loop.
        buffer_.reset(new (std::nothrow) char[64 * 1024]);
        if (buffer_)
            (void)std::setvbuf(file_, buffer_.get(), _IOFBF, 64 * 1024);
        std::fprintf(file_,
                     "sequence,received_us,source_pts,kind,valid,cx,cy,area,radius,"
                     "quality,clipped,missed_frames,candidates,full_frame,rescanned,"
                     "win_x0,win_y0,win_x1,win_y1,admission\n");
        std::fflush(file_);
        return true;
    }

    void row(const camera::FrameView& view,
             const detection::DetectResult* result,
             const char* admission) noexcept
    {
        if (file_ == nullptr)
            return;
        if (result == nullptr) {
            /* 识别关闭时仍然记录帧身份与放行决定，保证 CSV 覆盖每一帧。 */
            std::fprintf(file_, "%llu,%lld,%llu,none,0,,,,,,,,,,,%s\n",
                         static_cast<unsigned long long>(view.sequence),
                         static_cast<long long>(view.received.count()),
                         static_cast<unsigned long long>(view.source_pts),
                         admission);
            return;
        }
        const auto& target = result->target;
        const auto& scan = result->scan;
        std::fprintf(file_,
                     "%llu,%lld,%llu,%s,%d,%.1f,%.1f,%u,%.2f,%.3f,%d,%u,%u,%d,%d,"
                     "%u,%u,%u,%u,%s\n",
                     static_cast<unsigned long long>(result->source.frame_id),
                     static_cast<long long>(result->source.received.count()),
                     static_cast<unsigned long long>(result->source.source_pts),
                     kind_text(result->kind), target.valid ? 1 : 0,
                     static_cast<double>(target.center.x),
                     static_cast<double>(target.center.y), target.area,
                     static_cast<double>(target.radius),
                     static_cast<double>(target.quality), target.clipped ? 1 : 0,
                     result->missed_frames, scan.candidates,
                     scan.full_frame ? 1 : 0, scan.rescanned ? 1 : 0,
                     scan.search_window.x, scan.search_window.y,
                     scan.search_window.x + scan.search_window.width,
                     scan.search_window.y + scan.search_window.height,
                     admission);
    }

    void flush() noexcept
    {
        if (file_ == nullptr)
            return;
        std::fflush(file_);
    }

    void close() noexcept
    {
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
};

/* 停止输入之后的收尾：把录像线程归还的租约在采集线程释放干净。 */
void drain_releases(recorder::Recorder& rec, Logger& log) noexcept
{
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (rec.pending_releases() == 0) {
            log.line("level=INFO module=app op=shutdown step=leases_drained "
                     "pending=0\n");
            return;
        }
        (void)rec.drain_releases();
        sleep_ms(10);
    }
    log.line("level=WARN module=app op=shutdown step=leases_drained pending=%u "
             "action=continue note=drain_incomplete\n",
             rec.pending_releases());
}

int run_application(const Args& args) noexcept
{
    Logger log;
    if (!log.open(args.log_path)) {
        ensure_parent(args.log_path);
        (void)log.open(args.log_path);
    }
    log.line("level=INFO module=app op=start version=2 acq=%ux%u@%u out=%ux%u "
             "csi=%u mirror=%u exposure=%s exp_us=%u vision=%d record=%d "
             "rec_fps=%u rec_keep=%u rec_format=%s rec_dir=%s threshold=%u seconds=%u "
             "log=%s\n",
             args.sensor_width, args.sensor_height, args.sensor_fps,
             args.output_width, args.output_height, args.csi_port, args.mirror,
             args.auto_exposure ? "auto" : "fixed", args.exposure_us,
             args.vision_on ? 1 : 0, args.record_on ? 1 : 0, args.record_fps,
             args.record_keep,
             args.pbm ? "pbm" : "pgm", args.rec_dir, args.threshold, args.seconds,
             args.log_path);

    struct sigaction action {};
    action.sa_handler = on_signal;
    (void)sigaction(SIGINT, &action, nullptr);
    (void)sigaction(SIGTERM, &action, nullptr);

    camera::Config camera_config{};
    camera_config.csi_port = args.csi_port;
    camera_config.sensor_width = args.sensor_width;
    camera_config.sensor_height = args.sensor_height;
    camera_config.sensor_fps = args.sensor_fps;
    camera_config.output_width = args.output_width;
    camera_config.output_height = args.output_height;
    camera_config.capture_buffers = kCaptureBuffers;
    camera_config.channel_buffers = kChannelBuffers;
    camera_config.exposure_mode = args.auto_exposure ? camera::ExposureMode::kAuto
                                                     : camera::ExposureMode::kFixed;
    camera_config.initial_exposure_us = args.exposure_us;
    camera_config.mirror = args.mirror;

    auto created = camera::k230::create<kLeaseCapacity>(camera_config);
    if (!created) {
        log_camera_error(log, "camera_create", "check_sensor_mode_and_exposure",
                         *created.error());
        return 1;
    }
    std::unique_ptr<camera::ICamera> cam = std::move(created.value());
    log.line("level=INFO module=app op=camera_create result=ok capacity=%u "
             "sensor=%ux%u@%u out=%ux%u exposure=%s applied_us=%u\n",
             cam->capacity(), cam->caps().sensor_width, cam->caps().sensor_height,
             cam->caps().sensor_fps, cam->caps().output_width,
             cam->caps().output_height,
             cam->caps().exposure_mode == camera::ExposureMode::kAuto ? "auto"
                                                                        : "fixed",
             camera_config.initial_exposure_us);

    detection::DetectionConfig detect_config{};
    detect_config.threshold = static_cast<std::uint8_t>(args.threshold);
    detection::RoiDetector detector{detect_config};
    const auto max_width = std::max<std::uint32_t>(cam->caps().output_width,
                                                   args.sensor_width);
    const auto max_height = std::max<std::uint32_t>(cam->caps().output_height,
                                                    args.sensor_height);
    if (args.vision_on) {
        auto prepared = detector.Prepare(max_width, max_height);
        if (!prepared) {
            log.line("level=ERROR module=app op=detector_prepare code=%d "
                     "max=%ux%u action=exit\n",
                     static_cast<int>(prepared.error()), max_width, max_height);
            return 1;
        }
        log.line("level=INFO module=app op=detector_prepare result=ok max=%ux%u "
                 "threshold=%u\n",
                 max_width, max_height, args.threshold);
    }

    recorder::Config recorder_config{};
    recorder_config.width = cam->caps().output_width;
    recorder_config.height = cam->caps().output_height;
    recorder_config.fps = args.record_fps;
    recorder_config.keep_sessions = args.record_keep;
    recorder_config.queue_slots = 2;
    recorder_config.image_format = args.pbm ? recorder::ImageFormat::kPbm
                                            : recorder::ImageFormat::kPgm;
    recorder_config.pbm_threshold = static_cast<std::uint8_t>(args.threshold);
    recorder_config.output_directory = std::string_view{args.rec_dir};
    recorder::Recorder rec{recorder_config};

    CsvWriter csv;
    if (args.det_csv[0] != '\0' && !csv.open(args.det_csv)) {
        log.line("level=WARN module=app op=csv_open result=failed path=%s "
                 "action=continue\n",
                 args.det_csv);
    } else if (args.det_csv[0] != '\0') {
        log.line("level=INFO module=app op=csv_open result=ok path=%s\n",
                 args.det_csv);
    }

    auto started = cam->start();
    if (!started) {
        log_camera_error(log, "camera_start", "exit", *started.error());
        csv.close();
        return 1;
    }
    log.line("level=INFO module=app op=camera_start result=ok state=%u\n",
             static_cast<unsigned>(cam->state()));

    if (args.record_on) {
        /* Recorder 只会单层 mkdir 自己的根目录；父目录（如 /sdcard/app）由这里兜底。 */
        ensure_dir(args.rec_dir);
        if (!rec.start()) {
            log.line("level=ERROR module=app op=recorder_start result=failed "
                     "code=%s system_errno=%d path=%s action=stop_input\n",
                     recorder::Recorder::error_text(rec.last_error()).data(),
                     rec.last_system_error(), rec.last_error_path());
            (void)cam->stop();
            csv.close();
            return 1;
        }
        const auto stats = rec.stats();
        log.line("level=INFO module=app op=recorder_start result=ok dir=%s fps=%u "
                 "queued=%u\n",
                 args.rec_dir, recorder_config.fps, stats.queued);
    }

    Counters counters{};
    const auto start_time = std::chrono::steady_clock::now();
    const auto deadline = start_time + std::chrono::seconds{args.seconds};
    auto next_status = start_time + std::chrono::milliseconds{args.status_ms};
    std::uint32_t consecutive_acquire_errors = 0;
    bool exit_by_deadline = false;
    auto timing_begin = start_time;
    std::uint64_t timing_frames = 0;
    StageTiming acquire_timing{}, detect_timing{}, pipeline_timing{}, telemetry_timing{};

    while (g_stop == 0) {
        if (args.seconds > 0 && std::chrono::steady_clock::now() >= deadline) {
            exit_by_deadline = true;
            break;
        }

        const auto acquire_begin = std::chrono::steady_clock::now();
        auto acquired = cam->acquire(std::chrono::milliseconds{args.acquire_ms});
        acquire_timing.observe(acquire_begin);
        if (!acquired) {
            counters.acquire_errors++;
            consecutive_acquire_errors++;
            log_camera_error(log, "camera_acquire", "retry",
                             *acquired.error());
            if (consecutive_acquire_errors >= args.max_acquire_errors) {
                log.line("level=ERROR module=app op=camera_acquire "
                         "result=abort consecutive=%u limit=%u\n",
                         consecutive_acquire_errors, args.max_acquire_errors);
                break;
            }
            continue;
        }
        consecutive_acquire_errors = 0;

        if (acquired->status == camera::AcquireStatus::kNoFrame) {
            /* 正常否定结果：不是错误，只计数。 */
            counters.no_frame++;
            (void)rec.drain_releases();
            continue;
        }
        if (acquired->status == camera::AcquireStatus::kLeaseExhausted) {
            /* 容量性丢弃：驱动从未被触碰，这一帧在驱动池里自生自灭。 */
            counters.lease_exhausted++;
            (void)rec.drain_releases();
            continue;
        }

        counters.frames++;
        auto& lease = acquired->lease;
        // Keep metadata valid after release or transfer to the recording worker.
        const auto view = lease.view();

        const detection::DetectResult* result_ptr = nullptr;
        detection::DetectResult result{};
        if (args.vision_on) {
            counters.detect_calls++;
            const auto detect_begin = std::chrono::steady_clock::now();
            auto detected = detector.Detect(view);
            detect_timing.observe(detect_begin);
            if (!detected) {
                counters.detect_errors++;
                log.line("level=WARN module=app op=detect code=%d sequence=%llu "
                         "action=skip_frame\n",
                         static_cast<int>(detected.error()),
                         static_cast<unsigned long long>(view.sequence));
            } else {
                result = detected.value();
                result_ptr = &result;
                if (result.target.valid)
                    counters.target_hits++;
            }
        }

        /* 所有权：只有 kAccepted 会把租约转给 Recorder。 */
        const auto pipeline_begin = std::chrono::steady_clock::now();
        const char* admission = "off";
        if (args.record_on) {
            const auto decision = rec.try_submit(std::move(lease));
            admission = admission_text(decision);
            switch (decision) {
                case recorder::Admission::kAccepted: counters.rec_accepted++; break;
                case recorder::Admission::kDownsampled: counters.rec_downsampled++; break;
                case recorder::Admission::kBusy: counters.rec_busy++; break;
                case recorder::Admission::kInvalidFrame: counters.rec_invalid++; break;
                case recorder::Admission::kStopped: break;
            }
            if (decision != recorder::Admission::kAccepted) {
                auto released = lease.release();
                if (released)
                    counters.release_ok++;
                else {
                    counters.release_errors++;
                    log_camera_error(log, "camera_release", "count_and_continue",
                                     *released.error());
                }
            }
        } else {
            auto released = lease.release();
            if (released)
                counters.release_ok++;
            else {
                counters.release_errors++;
                log_camera_error(log, "camera_release", "count_and_continue",
                                 *released.error());
            }
        }

        csv.row(view, result_ptr, admission);
        (void)rec.drain_releases();
        pipeline_timing.observe(pipeline_begin);

        if (std::chrono::steady_clock::now() >= next_status) {
            const auto telemetry_begin = std::chrono::steady_clock::now();
            next_status = std::chrono::steady_clock::now()
                + std::chrono::milliseconds{args.status_ms};
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - start_time)
                                        .count();
            const auto stats = rec.stats();
            log.begin_batch();
            log.line("level=INFO module=app op=status elapsed_s=%lld frames=%llu "
                     "fps=%.1f no_frame=%llu lease_exhausted=%llu target=%llu "
                     "detect_err=%llu rec_input=%llu rec_acc=%llu rec_down=%llu "
                     "rec_busy=%llu rec_queued=%u rec_pending=%u rec_written=%llu "
                     "rec_event_dropped=%llu release_err=%llu run_hw=%u\n",
                     static_cast<long long>(elapsed_ms / 1000),
                     static_cast<unsigned long long>(counters.frames),
                     elapsed_ms > 0
                         ? static_cast<double>(counters.frames) * 1000.0
                             / static_cast<double>(elapsed_ms)
                         : 0.0,
                     static_cast<unsigned long long>(counters.no_frame),
                     static_cast<unsigned long long>(counters.lease_exhausted),
                     static_cast<unsigned long long>(counters.target_hits),
                     static_cast<unsigned long long>(counters.detect_errors),
                     static_cast<unsigned long long>(stats.input),
                     static_cast<unsigned long long>(stats.accepted),
                     static_cast<unsigned long long>(stats.downsampled),
                     static_cast<unsigned long long>(stats.busy), stats.queued,
                     stats.release_pending,
                     static_cast<unsigned long long>(stats.written),
                     static_cast<unsigned long long>(stats.event_dropped),
                     static_cast<unsigned long long>(counters.release_errors),
                     detector.run_high_water());
            const auto window_us = std::chrono::duration_cast<std::chrono::microseconds>(
                telemetry_begin - timing_begin).count();
            log.line("level=INFO module=app op=timing window_ms=%lld fps=%.1f "
                     "acquire_us=%.1f acquire_max_us=%llu detect_us=%.1f detect_max_us=%llu "
                     "pipeline_us=%.1f pipeline_max_us=%llu telemetry_us=%.1f "
                     "telemetry_max_us=%llu note=acquire_includes_wait\n",
                     static_cast<long long>(window_us / 1000),
                     window_us > 0 ? static_cast<double>(counters.frames - timing_frames)
                                         * 1000000.0 / static_cast<double>(window_us) : 0.0,
                     acquire_timing.average_us(),
                     static_cast<unsigned long long>(acquire_timing.max_us),
                     detect_timing.average_us(),
                     static_cast<unsigned long long>(detect_timing.max_us),
                     pipeline_timing.average_us(),
                     static_cast<unsigned long long>(pipeline_timing.max_us),
                     telemetry_timing.average_us(),
                     static_cast<unsigned long long>(telemetry_timing.max_us));
            log.end_batch();
            csv.flush();
            timing_begin = telemetry_begin;
            timing_frames = counters.frames;
            acquire_timing = {};
            detect_timing = {};
            pipeline_timing = {};
            telemetry_timing = {};
            // Report this status write/flush cost in the following window.
            telemetry_timing.observe(telemetry_begin);
        }
    }

    /* ---------------- 收尾：停止输入 → 停消费者 → 排空 → 摘要 → 释放 SDK ---- */
    log.line("level=INFO module=app op=shutdown step=input_stop reason=%s "
             "frames=%llu\n",
             g_stop != 0 ? "signal" : (exit_by_deadline ? "deadline" : "abort"),
             static_cast<unsigned long long>(counters.frames));

    if (args.record_on) {
        rec.stop();
        log.line("level=INFO module=app op=shutdown step=recorder_stopped "
                 "pending=%u\n",
                 rec.pending_releases());
    }
    drain_releases(rec, log);

    if (cam->state() == camera::State::kStarted) {
        auto stopped = cam->stop();
        if (!stopped) {
            log_camera_error(log, "camera_stop", "destroy_anyway", *stopped.error());
        } else {
            log.line("level=INFO module=app op=shutdown step=camera_stopped "
                     "result=ok\n");
        }
    }

    const auto stats = rec.stats();
    log.line("level=INFO module=app op=shutdown step=summary frames=%llu "
             "no_frame=%llu lease_exhausted=%llu acquire_errors=%llu "
             "target_hits=%llu detect_calls=%llu detect_errors=%llu "
             "rec_accepted=%llu rec_downsampled=%llu rec_busy=%llu rec_invalid=%llu "
             "rec_written=%llu rec_bytes=%llu rec_queue_hw=%u rec_event_dropped=%llu "
             "rec_copy_fail=%llu rec_write_fail=%llu rec_lease_release_fail=%llu "
             "release_ok=%llu release_errors=%llu run_hw=%u\n",
             static_cast<unsigned long long>(counters.frames),
             static_cast<unsigned long long>(counters.no_frame),
             static_cast<unsigned long long>(counters.lease_exhausted),
             static_cast<unsigned long long>(counters.acquire_errors),
             static_cast<unsigned long long>(counters.target_hits),
             static_cast<unsigned long long>(counters.detect_calls),
             static_cast<unsigned long long>(counters.detect_errors),
             static_cast<unsigned long long>(counters.rec_accepted),
             static_cast<unsigned long long>(counters.rec_downsampled),
             static_cast<unsigned long long>(counters.rec_busy),
             static_cast<unsigned long long>(counters.rec_invalid),
             static_cast<unsigned long long>(stats.written),
             static_cast<unsigned long long>(stats.bytes), stats.queue_high_water,
             static_cast<unsigned long long>(stats.event_dropped),
             static_cast<unsigned long long>(stats.copy_fail),
             static_cast<unsigned long long>(stats.write_fail),
             static_cast<unsigned long long>(stats.lease_release_fail),
             static_cast<unsigned long long>(counters.release_ok),
             static_cast<unsigned long long>(counters.release_errors),
             detector.run_high_water());

    csv.close();

    cam.reset(); /* deinit -> destroy -> media destroy */
    log.line("level=INFO module=app op=shutdown step=camera_destroyed result=ok\n");
    log.line("level=INFO module=app op=exit result=done\n");
    return 0;
}

} // namespace

int run(int argc, char* argv[]) noexcept
{
    auto args = parse_args(argc, argv);
    if (!args)
        return args.error();
    const int result = run_application(args.value());
    const char* marker = std::getenv("DART_EXIT_MARKER");
    if (marker != nullptr && marker[0] == '/' && std::strlen(marker) < 512) {
        const int fd = ::open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            char pid_text[32];
            const int length = std::snprintf(pid_text, sizeof(pid_text), "%ld\n",
                                             static_cast<long>(::getpid()));
            if (length > 0 && static_cast<std::size_t>(length) < sizeof(pid_text)) {
                (void)::write(fd, pid_text, static_cast<std::size_t>(length));
                (void)::fsync(fd);
            }
            ::close(fd);
        }
    }
    return result;
}

} // namespace dart::app
