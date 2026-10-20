#ifndef DART_STARTUP_RUN_CPP
#define DART_STARTUP_RUN_CPP

#include <utility>

#include "argument_parser.cpp"
#include "logging/logger.cpp"
#include "services.cpp"
#include "system/device_services.cpp"
#include "system/launcher_exit.cpp"
#include "system/run_control.cpp"

namespace dart::app {

template <typename ImuRunner>
inline int run_capture(
    const Arguments& args, platform::CameraFactory camera_factory, ImuRunner&& run_imu) noexcept {
    platform::RunControl control{args.seconds};
    logging::Logger log;
    (void)log.open(args.log_path);
    const platform::ImuOptions options{args.imu_on, args.imu_bus, args.imu_cs, args.imu_log};
    return run_imu(options, log, [&](imu::Session* session) noexcept {
        return run_services(args, camera_factory, control, log, session);
    });
}

template <typename ImuRunner>
inline int run_application(
    int argc, char* argv[], platform::CameraFactory camera_factory, ImuRunner&& run_imu) noexcept {
    auto args = parse_args(argc, argv);
    if (!args)
        return args.error();
    const int result = run_capture(args.value(), camera_factory, std::forward<ImuRunner>(run_imu));
    platform::notify_launcher_exit();
    return result;
}

} // namespace dart::app

#endif // DART_STARTUP_RUN_CPP
