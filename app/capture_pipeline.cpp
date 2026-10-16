#ifndef DART_APP_CAPTURE_PIPELINE_CPP
#define DART_APP_CAPTURE_PIPELINE_CPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <signal.h>
#include <string_view>
#include <utility>

#include "argument_parser.cpp"
#include "camera/k230/camera.cpp"
#include "detection_csv.cpp"
#include "directories.cpp"
#include "logger.cpp"
#include "monotonic_clock.cpp"
#include "runtime/imu/session.cpp"
#include "runtime/recordor/recorder.cpp"
#include "src/detector/roi.cpp"

namespace dart::app {

class CapturePipeline final {
private:
    struct StageTiming {
        std::uint64_t total_us = 0;
        std::uint64_t max_us = 0;
        std::uint64_t samples = 0;

        void observe(std::chrono::steady_clock::time_point begin) noexcept {
            const auto us =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - begin)
                                               .count());
            total_us += us;
            max_us = std::max(max_us, us);
            ++samples;
        }

        double average_us() const noexcept {
            return samples > 0 ? static_cast<double>(total_us) / static_cast<double>(samples) : 0.0;
        }
    };

    class StopSignal final {
    public:
        static void install() noexcept {
            stopped_ = 0;
            struct sigaction action{};
            action.sa_handler = &StopSignal::handle;
            (void)sigaction(SIGINT, &action, nullptr);
            (void)sigaction(SIGTERM, &action, nullptr);
        }
        static bool requested() noexcept { return stopped_ != 0; }

    private:
        static void handle(int) noexcept { stopped_ = 1; }
        inline static volatile sig_atomic_t stopped_ = 0;
    };

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

    static std::string_view operation_text(camera::Operation operation) noexcept {
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

    static void log_camera_error(
        Logger& log, const char* op, const char* action,
        const camera::CameraError& error) noexcept {
        const auto& context = error.context();
        log.line(
            "level=ERROR module=app op=%s code=%.*s native=%d detail=\"%.*s\" "
            "ctx_op=%.*s requested=%u observed=%u state=%u req_state=%u action=%s\n",
            op, static_cast<int>(error.category().size()), error.category().data(), error.code(),
            static_cast<int>(error.message().size()), error.message().data(),
            static_cast<int>(operation_text(context.operation).size()),
            operation_text(context.operation).data(), context.requested, context.observed,
            context.state, context.required_state, action);
    }

    static constexpr std::uint32_t kLeaseCapacity = 4;

    static constexpr std::uint32_t kChannelBuffers = 6;

    static constexpr std::uint32_t kCaptureBuffers = 4;

    static const char* admission_text(recorder::Admission admission) noexcept {
        switch (admission) {
        case recorder::Admission::kAccepted: return "accepted";
        case recorder::Admission::kDownsampled: return "downsampled";
        case recorder::Admission::kBusy: return "busy";
        case recorder::Admission::kStopped: return "stopped";
        case recorder::Admission::kInvalidFrame: return "invalid_frame";
        }
        return "unknown";
    }

    static void drain_recording_releases(recorder::Recorder& rec, Logger& log) noexcept {
        for (int attempt = 0; attempt < 500; ++attempt) {
            if (rec.pending_releases() == 0) {
                log.line(
                    "level=INFO module=app op=shutdown step=leases_drained "
                    "pending=0\n");
                return;
            }
            (void)rec.drain_releases();
            sleep_ms(10);
        }
        log.line(
            "level=WARN module=app op=shutdown step=leases_drained pending=%u "
            "action=continue note=drain_incomplete\n",
            rec.pending_releases());
    }

public:
    static int execute(const Arguments& args, imu::Session* imu_session = nullptr) noexcept {
        Logger log;
        if (!log.open(args.log_path)) {
            ensure_parent(args.log_path);
            (void)log.open(args.log_path);
        }
        log.line(
            "level=INFO module=app op=start version=2 acq=%ux%u@%u out=%ux%u "
            "csi=%u mirror=%u exposure=%s exp_us=%u vision=%d record=%d "
            "rec_fps=%u rec_keep=%u rec_format=%s rec_dir=%s threshold=%u seconds=%u "
            "log=%s\n",
            args.sensor_width, args.sensor_height, args.sensor_fps, args.output_width,
            args.output_height, args.csi_port, args.mirror, args.auto_exposure ? "auto" : "fixed",
            args.exposure_us, args.vision_on ? 1 : 0, args.record_on ? 1 : 0, args.record_fps,
            args.record_keep, args.pbm ? "pbm" : "pgm", args.rec_dir, args.threshold, args.seconds,
            args.log_path);

        StopSignal::install();

        camera::Config camera_config{};
        camera_config.csi_port = args.csi_port;
        camera_config.sensor_width = args.sensor_width;
        camera_config.sensor_height = args.sensor_height;
        camera_config.sensor_fps = args.sensor_fps;
        camera_config.output_width = args.output_width;
        camera_config.output_height = args.output_height;
        camera_config.capture_buffers = kCaptureBuffers;
        camera_config.channel_buffers = kChannelBuffers;
        camera_config.exposure_mode =
            args.auto_exposure ? camera::ExposureMode::kAuto : camera::ExposureMode::kFixed;
        camera_config.initial_exposure_us = args.exposure_us;
        camera_config.mirror = args.mirror;

        auto created = camera::k230::Camera<kLeaseCapacity>::create(camera_config);
        if (!created) {
            log_camera_error(
                log, "camera_create", "check_sensor_mode_and_exposure", created.error());
            return 1;
        }
        std::unique_ptr<camera::ICamera> cam = std::move(created.value());
        log.line(
            "level=INFO module=app op=camera_create result=ok capacity=%u "
            "sensor=%ux%u@%u out=%ux%u exposure=%s applied_us=%u\n",
            cam->capacity(), cam->caps().sensor_width, cam->caps().sensor_height,
            cam->caps().sensor_fps, cam->caps().output_width, cam->caps().output_height,
            cam->caps().exposure_mode == camera::ExposureMode::kAuto ? "auto" : "fixed",
            camera_config.initial_exposure_us);

        detection::RoiDetector::Config detect_config{};
        detect_config.threshold = static_cast<std::uint8_t>(args.threshold);
        detection::RoiDetector detector{detect_config};
        const auto max_width = std::max<std::uint32_t>(cam->caps().output_width, args.sensor_width);
        const auto max_height =
            std::max<std::uint32_t>(cam->caps().output_height, args.sensor_height);
        if (args.vision_on) {
            auto prepared = detector.Prepare(max_width, max_height);
            if (!prepared) {
                log.line(
                    "level=ERROR module=app op=detector_prepare code=%d "
                    "max=%ux%u action=exit\n",
                    static_cast<int>(prepared.error()), max_width, max_height);
                return 1;
            }
            log.line(
                "level=INFO module=app op=detector_prepare result=ok max=%ux%u "
                "threshold=%u\n",
                max_width, max_height, args.threshold);
        }

        recorder::Config recorder_config{};
        recorder_config.width = cam->caps().output_width;
        recorder_config.height = cam->caps().output_height;
        recorder_config.fps = args.record_fps;
        recorder_config.keep_sessions = args.record_keep;
        recorder_config.queue_slots = 2;
        recorder_config.image_format =
            args.pbm ? recorder::ImageFormat::kPbm : recorder::ImageFormat::kPgm;
        recorder_config.pbm_threshold = static_cast<std::uint8_t>(args.threshold);
        recorder_config.output_directory = std::string_view{args.rec_dir};
        recorder::Recorder rec{recorder_config};

        DetectionCsv csv;
        if (args.det_csv[0] != '\0' && !csv.open(args.det_csv)) {
            log.line(
                "level=WARN module=app op=csv_open result=failed path=%s "
                "action=continue\n",
                args.det_csv);
        } else if (args.det_csv[0] != '\0') {
            log.line("level=INFO module=app op=csv_open result=ok path=%s\n", args.det_csv);
        }

        auto started = cam->start();
        if (!started) {
            log_camera_error(log, "camera_start", "exit", started.error());
            csv.close();
            return 1;
        }
        log.line(
            "level=INFO module=app op=camera_start result=ok state=%u\n",
            static_cast<unsigned>(cam->state()));

        if (args.record_on) {
            /* Recorder 只会单层 mkdir 自己的根目录；父目录（如 /sdcard/app）由这里兜底。 */
            ensure_dir(args.rec_dir);
            if (!rec.start()) {
                log.line(
                    "level=ERROR module=app op=recorder_start result=failed "
                    "code=%s system_errno=%d path=%s action=stop_input\n",
                    recorder::Recorder::error_text(rec.last_error()).data(),
                    rec.last_system_error(), rec.last_error_path());
                (void)cam->stop();
                csv.close();
                return 1;
            }
            const auto stats = rec.stats();
            log.line(
                "level=INFO module=app op=recorder_start result=ok dir=%s fps=%u "
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

        while (!StopSignal::requested()) {
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
                log_camera_error(
                    log, "camera_acquire", "retry", acquired.error());
                if (consecutive_acquire_errors >= args.max_acquire_errors) {
                    log.line(
                        "level=ERROR module=app op=camera_acquire "
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
                    log.line(
                        "level=WARN module=app op=detect code=%d sequence=%llu "
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
                        log_camera_error(
                            log, "camera_release", "count_and_continue", released.error());
                    }
                }
            } else {
                auto released = lease.release();
                if (released)
                    counters.release_ok++;
                else {
                    counters.release_errors++;
                    log_camera_error(
                        log, "camera_release", "count_and_continue", released.error());
                }
            }

            csv.row(view, result_ptr, admission);
            (void)rec.drain_releases();
            pipeline_timing.observe(pipeline_begin);

            if (std::chrono::steady_clock::now() >= next_status) {
                const auto telemetry_begin = std::chrono::steady_clock::now();
                next_status =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds{args.status_ms};
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - start_time)
                                            .count();
                const auto stats = rec.stats();
                log.begin_batch();
                log.line(
                    "level=INFO module=app op=status elapsed_s=%lld frames=%llu "
                    "fps=%.1f no_frame=%llu lease_exhausted=%llu target=%llu "
                    "detect_err=%llu rec_input=%llu rec_acc=%llu rec_down=%llu "
                    "rec_busy=%llu rec_queued=%u rec_pending=%u rec_written=%llu "
                    "rec_event_dropped=%llu release_err=%llu run_hw=%u\n",
                    static_cast<long long>(elapsed_ms / 1000),
                    static_cast<unsigned long long>(counters.frames),
                    elapsed_ms > 0 ? static_cast<double>(counters.frames) * 1000.0
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
                    stats.release_pending, static_cast<unsigned long long>(stats.written),
                    static_cast<unsigned long long>(stats.event_dropped),
                    static_cast<unsigned long long>(counters.release_errors),
                    detector.run_high_water());
                const auto window_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                           telemetry_begin - timing_begin)
                                           .count();
                log.line(
                    "level=INFO module=app op=timing window_ms=%lld fps=%.1f "
                    "acquire_us=%.1f acquire_max_us=%llu detect_us=%.1f detect_max_us=%llu "
                    "pipeline_us=%.1f pipeline_max_us=%llu telemetry_us=%.1f "
                    "telemetry_max_us=%llu note=acquire_includes_wait\n",
                    static_cast<long long>(window_us / 1000),
                    window_us > 0 ? static_cast<double>(counters.frames - timing_frames) * 1000000.0
                                        / static_cast<double>(window_us)
                                  : 0.0,
                    acquire_timing.average_us(),
                    static_cast<unsigned long long>(acquire_timing.max_us),
                    detect_timing.average_us(),
                    static_cast<unsigned long long>(detect_timing.max_us),
                    pipeline_timing.average_us(),
                    static_cast<unsigned long long>(pipeline_timing.max_us),
                    telemetry_timing.average_us(),
                    static_cast<unsigned long long>(telemetry_timing.max_us));
                if (imu_session != nullptr) {
                    const auto estimate = imu_session->current();
                    const auto imu_stats = imu_session->stats();
                    log.line(
                        "level=INFO module=imu op=status timestamp_ns=%llu generation=%llu "
                        "quality_events=%u accepted=%d integrated=%d reference=%d "
                        "samples=%llu read_errors=%llu dropped=%llu io_errors=%llu\n",
                        static_cast<unsigned long long>(estimate.sample.timestamp_ns),
                        static_cast<unsigned long long>(estimate.generation), estimate.quality_events,
                        estimate.sample_accepted, estimate.integrated, estimate.has_reference_frame,
                        static_cast<unsigned long long>(imu_stats.samples),
                        static_cast<unsigned long long>(imu_stats.read_errors),
                        static_cast<unsigned long long>(imu_stats.dropped),
                        static_cast<unsigned long long>(imu_stats.io_errors));
                }
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
        log.line(
            "level=INFO module=app op=shutdown step=input_stop reason=%s "
            "frames=%llu\n",
            StopSignal::requested() ? "signal" : (exit_by_deadline ? "deadline" : "abort"),
            static_cast<unsigned long long>(counters.frames));

        if (args.record_on) {
            rec.stop();
            log.line(
                "level=INFO module=app op=shutdown step=recorder_stopped "
                "pending=%u\n",
                rec.pending_releases());
        }
        drain_recording_releases(rec, log);

        if (cam->state() == camera::State::kStarted) {
            auto stopped = cam->stop();
            if (!stopped) {
                log_camera_error(
                    log, "camera_stop", "destroy_anyway", stopped.error());
            } else {
                log.line(
                    "level=INFO module=app op=shutdown step=camera_stopped "
                    "result=ok\n");
            }
        }

        const auto stats = rec.stats();
        log.line(
            "level=INFO module=app op=shutdown step=summary frames=%llu "
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
            static_cast<unsigned long long>(counters.release_errors), detector.run_high_water());

        csv.close();

        cam.reset(); /* deinit -> destroy -> media destroy */
        log.line("level=INFO module=app op=shutdown step=camera_destroyed result=ok\n");
        log.line("level=INFO module=app op=exit result=done\n");
        return 0;
    }
};

} // namespace dart::app

#endif // DART_APP_CAPTURE_PIPELINE_CPP
