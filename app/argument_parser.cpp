#ifndef DART_APP_ARGUMENT_PARSER_CPP
#define DART_APP_ARGUMENT_PARSER_CPP

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string_view>

#include "runtime/recordor/config.cpp"

namespace dart::app {

class Arguments {
public:
    std::uint32_t seconds = 0;      /* 0 = 跑到信号 */
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
    char det_csv[512] = "";         /* 空 = 关闭 */
    char log_path[512] = "/sdcard/app/logs/dart_app.log";
    bool imu_on = false;
    int imu_bus = -1;
    int imu_cs = -2; /* -2 = 未配置；-1 = HAL 默认片选。 */
    char imu_log[512] = "/sdcard/app/logs/imu.log";
    std::uint32_t status_ms = 1000;
    std::uint32_t acquire_ms = 200;
    std::uint32_t max_acquire_errors = 8;
    std::uint32_t threshold = 50;
};

inline constexpr std::string_view kDefaultLog = "/sdcard/app/logs/dart_app.log";

inline constexpr std::string_view kDefaultRecordDir = "/sdcard/app/records";

inline std::int64_t parse_int(std::string_view text) noexcept {
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
inline bool
    parse_size(std::string_view text, std::uint32_t& width, std::uint32_t& height) noexcept {
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

inline bool parse_on_off(std::string_view text, bool& out) noexcept {
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

inline bool copy_path(char* out, std::size_t cap, std::string_view text) noexcept {
    if (text.size() >= cap)
        return false;
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return true;
}

inline void usage(const char* argv0) {
    std::printf("用法: %s [选项]\n\n", argv0);
    std::printf("  --seconds N      有界运行秒数，0 = 跑到 SIGINT/SIGTERM（默认 0）\n");
    std::printf("  --acq WxH        传感器模式（默认 1920x1080，需精确命中，无静默回退）\n");
    std::printf("  --fps N          传感器帧率（默认 30）\n");
    std::printf(
        "  --out WxH        输出尺寸（默认跟随 --acq；宽 32 对齐、高偶数、不上采样）\n");
    std::printf("  --csi N          CSI 端口 0..2（默认 0）\n");
    std::printf("  --mirror N       0 无 / 1 水平 / 2 垂直 / 3 双向（默认 0）\n");
    std::printf("  --exp US         固定曝光微秒（默认 250；0 = 未定，会阻塞取帧）\n");
    std::printf("  --ae             自动曝光（与 --exp 互斥）\n");
    std::printf("  --vision on|off  识别开关（默认 on）\n");
    std::printf("  --record on|off  录像开关（默认 on）\n");
    std::printf(
        "  --rec-fps N      目标录像帧率，按时间采样（默认 %u FPS）\n", recorder::Config{}.fps);
    std::printf("  --rec-keep N     只保留最近 N 轮录像，0 = 不清理（默认 0）\n");
    std::printf("  --rec-format F   pgm（灰度，默认）| pbm（按阈值二值化）\n");
    std::printf(
        "  --rec-dir DIR    录像根目录（默认 %.*s）\n",
        static_cast<int>(kDefaultRecordDir.size()), kDefaultRecordDir.data());
    std::printf("  --det-csv PATH   识别结果 CSV；off 关闭（默认关闭）\n");
    std::printf("  --threshold N    二值化阈值（默认 50，UNVALIDATED）\n");
    std::printf(
        "  --log PATH       结构化日志（默认 %.*s）\n", static_cast<int>(kDefaultLog.size()),
        kDefaultLog.data());
    std::printf("  --status-ms N    状态行间隔（默认 1000）\n");
    std::printf("  --imu on|off     IMU 独立采样与日志（默认 off）\n");
    std::printf("  --imu-bus N      SPI 总线 0..2，启用 IMU 时必填\n");
    std::printf("  --imu-cs N       片选引脚 0..63，-1 使用 HAL 默认，启用 IMU 时必填\n");
    std::printf("  --imu-log PATH   原始/积分数据日志（默认 /sdcard/app/logs/imu.log）\n");
    std::printf("  --acquire-ms N   单次取帧超时（默认 200）\n");
    std::printf("  --max-acquire-errors N  连续取帧错误上限，超过即退出（默认 8）\n");
    std::printf("  -h, --help       显示本帮助\n");
}

/* 失败值是退出码；帮助返回 0，参数非法返回 2，消息已打印。 */
inline std::expected<Arguments, int> parse_args(int argc, char* argv[]) {
    Arguments args;
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
        if (a == "--imu-bus" || a == "--imu-cs") {
            if (!need_value(value)) {
                std::fprintf(
                    stderr, "%s: %.*s 缺少参数\n", argv[0], static_cast<int>(a.size()), a.data());
                return std::unexpected<int>(2);
            }
            const auto n = a == "--imu-cs" && value == "-1" ? -1 : parse_int(value);
            if ((n < 0 && !(a == "--imu-cs" && value == "-1"))
                || n > (a == "--imu-bus" ? 2 : 63)) {
                std::fprintf(
                    stderr, "%s: %.*s 超出范围\n", argv[0], static_cast<int>(a.size()), a.data());
                return std::unexpected<int>(2);
            }
            if (a == "--imu-bus")
                args.imu_bus = static_cast<int>(n);
            else
                args.imu_cs = static_cast<int>(n);
            continue;
        }
        if (a == "--seconds" || a == "--fps" || a == "--csi" || a == "--mirror" || a == "--exp"
            || a == "--rec-fps" || a == "--rec-keep" || a == "--threshold" || a == "--status-ms"
            || a == "--acquire-ms" || a == "--max-acquire-errors") {
            if (!need_value(value)) {
                std::fprintf(
                    stderr, "%s: %.*s 缺少参数\n", argv[0], static_cast<int>(a.size()),
                    a.data());
                return std::unexpected<int>(2);
            }
            const auto n = parse_int(value);
            if (n < 0) {
                std::fprintf(
                    stderr, "%s: %.*s 需要非负整数，得到 %.*s\n", argv[0],
                    static_cast<int>(a.size()), a.data(), static_cast<int>(value.size()),
                    value.data());
                return std::unexpected<int>(2);
            }
            if (a == "--seconds")
                args.seconds = static_cast<std::uint32_t>(n);
            else if (a == "--fps")
                args.sensor_fps = static_cast<std::uint32_t>(n);
            else if (a == "--csi")
                args.csi_port = static_cast<std::uint32_t>(n);
            else if (a == "--mirror")
                args.mirror = static_cast<std::uint32_t>(n);
            else if (a == "--exp")
                args.exposure_us = static_cast<std::uint32_t>(n);
            else if (a == "--rec-fps") {
                if (n == 0 || n > 1000000) {
                    std::fprintf(stderr, "%s: --rec-fps 必须在 1..1000000\n", argv[0]);
                    return std::unexpected<int>(2);
                }
                args.record_fps = static_cast<std::uint32_t>(n);
            } else if (a == "--rec-keep")
                args.record_keep = static_cast<std::uint32_t>(n);
            else if (a == "--threshold")
                args.threshold = static_cast<std::uint32_t>(n);
            else if (a == "--status-ms")
                args.status_ms = static_cast<std::uint32_t>(n);
            else if (a == "--acquire-ms")
                args.acquire_ms = static_cast<std::uint32_t>(n);
            else
                args.max_acquire_errors = static_cast<std::uint32_t>(n);
            continue;
        }
        if (a == "--acq" || a == "--out") {
            if (!need_value(value)) {
                std::fprintf(
                    stderr, "%s: %.*s 缺少参数\n", argv[0], static_cast<int>(a.size()),
                    a.data());
                return std::unexpected<int>(2);
            }
            std::uint32_t w = 0;
            std::uint32_t h = 0;
            if (!parse_size(value, w, h)) {
                std::fprintf(
                    stderr, "%s: %.*s 需要 WxH，得到 %.*s\n", argv[0],
                    static_cast<int>(a.size()), a.data(), static_cast<int>(value.size()),
                    value.data());
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
        if (a == "--vision" || a == "--record" || a == "--imu") {
            if (!need_value(value)) {
                std::fprintf(
                    stderr, "%s: %.*s 缺少参数\n", argv[0], static_cast<int>(a.size()),
                    a.data());
                return std::unexpected<int>(2);
            }
            bool flag = false;
            if (!parse_on_off(value, flag)) {
                std::fprintf(
                    stderr, "%s: %.*s 需要 on|off\n", argv[0], static_cast<int>(a.size()),
                    a.data());
                return std::unexpected<int>(2);
            }
            if (a == "--vision")
                args.vision_on = flag;
            else if (a == "--imu")
                args.imu_on = flag;
            else
                args.record_on = flag;
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
            if (value == "pgm")
                args.pbm = false;
            else if (value == "pbm")
                args.pbm = true;
            else {
                std::fprintf(stderr, "%s: --rec-format 需要 pgm|pbm\n", argv[0]);
                return std::unexpected<int>(2);
            }
            continue;
        }
        if (a == "--rec-dir" || a == "--det-csv" || a == "--log" || a == "--imu-log") {
            if (!need_value(value)) {
                std::fprintf(
                    stderr, "%s: %.*s 缺少参数\n", argv[0], static_cast<int>(a.size()),
                    a.data());
                return std::unexpected<int>(2);
            }
            if (a == "--imu-log") {
                if (value.empty() || !copy_path(args.imu_log, sizeof(args.imu_log), value)) {
                    std::fprintf(stderr, "%s: --imu-log 路径为空或过长\n", argv[0]);
                    return std::unexpected<int>(2);
                }
            } else if (a == "--rec-dir") {
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
        std::fprintf(
            stderr, "%s: 未知选项 %.*s（-h 查看用法）\n", argv[0], static_cast<int>(a.size()),
            a.data());
        return std::unexpected<int>(2);
    }

    if (args.imu_on && (args.imu_bus < 0 || args.imu_cs < -1)) {
        std::fprintf(stderr, "%s: --imu on 需要显式指定 --imu-bus 和 --imu-cs\n", argv[0]);
        return std::unexpected<int>(2);
    }
    if (args.output_width == 0 || args.output_height == 0) {
        args.output_width = args.sensor_width;
        args.output_height = args.sensor_height;
    }
    if (args.auto_exposure && args.exposure_us != 0) {
        std::fprintf(
            stderr, "%s: --ae 与 --exp 互斥（--ae 时 initial_exposure_us 必须为 0）\n",
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

} // namespace dart::app

#endif // DART_APP_ARGUMENT_PARSER_CPP
