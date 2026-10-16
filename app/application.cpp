#ifndef DART_APP_APPLICATION_CPP
#define DART_APP_APPLICATION_CPP

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "argument_parser.cpp"
#include "capture_pipeline.cpp"
#include "directories.cpp"
#include "logger.cpp"
#include "runtime/imu/session.cpp"
#ifdef DART_APP_IMU_K230
#include "imu/k230/iim42622.cpp"
#endif

namespace dart::app {

inline void notify_launcher_exit() noexcept {
    const char* marker = std::getenv("DART_EXIT_MARKER");
    if (marker != nullptr && marker[0] == '/' && std::strlen(marker) < 512) {
        const int fd = ::open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            char pid_text[32];
            const int length = std::snprintf(
                pid_text, sizeof(pid_text), "%ld\n", static_cast<long>(::getpid()));
            if (length > 0 && static_cast<std::size_t>(length) < sizeof(pid_text)) {
                (void)::write(fd, pid_text, static_cast<std::size_t>(length));
                (void)::fsync(fd);
            }
            ::close(fd);
        }
    }
}

inline int run_capture(const Arguments& args) noexcept {
    if (!args.imu_on)
        return CapturePipeline::execute(args);
#ifdef DART_APP_IMU_K230
    /* Explicit bus/CS; remaining acquisition defaults are UNVALIDATED.
     * 1 kHz ODR/polling, +/-16 g, +/-2000 deg/s, zero configured gyro bias. */
    dart_iim42622_config_t config{};
    config.bus = args.imu_bus;
    config.chip_select_pin = args.imu_cs;
    config.bus_hz = 1'000'000;
    config.timeout_ms = 20;
    config.output_data_rate = 0x06;
    imu::k230::Iim42622 device{config};
    const auto initialized = device.initialize();
    if (!initialized) {
        std::fprintf(
            stderr, "IMU initialize failed: code=%u\n", static_cast<unsigned>(initialized.error()));
        return 1;
    }
    ensure_parent(args.imu_log);
    imu::Session session{device};
    if (!session.start(args.imu_log, 1000, true)) {
        std::fprintf(stderr, "IMU log start failed: path=%s\n", args.imu_log);
        return 1;
    }
    Logger log;
    ensure_parent(args.log_path);
    (void)log.open(args.log_path);
    log.line(
        "level=INFO module=imu op=start bus=%d cs=%d bus_hz=%u timeout_ms=%u "
        "odr=0x%02x accel_range=%u gyro_range=%u gyro_bias_rad_s=0,0,0 "
        "period_us=1000 initial_seed=stationary_identity parameters=UNVALIDATED path=%s\n",
        config.bus, config.chip_select_pin, config.bus_hz, config.timeout_ms,
        config.output_data_rate, config.accelerometer_range, config.gyroscope_range, args.imu_log);
    // Consumers must share this session's current()/seed()/reset() methods.
    const int result = CapturePipeline::execute(args, &session);
    session.stop();
    const auto stats = session.stats();
    log.line(
        "level=%s module=imu op=stop samples=%llu read_errors=%llu written=%llu "
        "dropped=%llu io_errors=%llu path=%s\n",
        stats.io_errors > 0 ? "ERROR" : "INFO",
        static_cast<unsigned long long>(stats.samples),
        static_cast<unsigned long long>(stats.read_errors),
        static_cast<unsigned long long>(stats.written),
        static_cast<unsigned long long>(stats.dropped),
        static_cast<unsigned long long>(stats.io_errors), args.imu_log);
    return result != 0 || stats.io_errors > 0 ? 1 : 0;
#else
    std::fprintf(stderr, "IMU requires a build with DART_IMU_K230=ON\n");
    return 1;
#endif
}

inline int run_application(int argc, char* argv[]) noexcept {
    auto args = parse_args(argc, argv);
    if (!args)
        return args.error();
    const int result = run_capture(args.value());
    notify_launcher_exit();
    return result;
}

} // namespace dart::app

#endif // DART_APP_APPLICATION_CPP
