# Concrete runtime services and process supervision, outside the L4 domain.
# Module subdirectories contain sources and README files; targets live here.

# IMU acquisition, shared estimator state and asynchronous raw/estimate log.
add_library(dart_imu_logging INTERFACE)
target_link_libraries(dart_imu_logging INTERFACE dart_imu Threads::Threads)
target_include_directories(dart_imu_logging INTERFACE "${PROJECT_SOURCE_DIR}")
file(GLOB_RECURSE _imu_runtime_classes CONFIGURE_DEPENDS "imu/*.cpp")
add_library(dart_imu_logging_checks OBJECT ${_imu_runtime_classes})
target_link_libraries(dart_imu_logging_checks PRIVATE dart_imu_logging)

# Finite board diagnostic for initialization timing comparisons. Run alone.
if(DART_IMU_K230)
    add_executable(imu_init_probe imu/init_probe.c)
    target_compile_options(imu_init_probe PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(imu_init_probe PRIVATE dart_iim42652 k230_rtos_app m)
    set_target_properties(imu_init_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/imu_init_probe")
endif()

# ---------------------------------------------------------------------------
# launcher：开机自启动器（平台无关）
#
# 只做"读清单 → fork/execv 拉起 → 监督回收"，不认识相机、录像或任何 SDK。
# host 也能构建与运行，便于在电脑上用假清单验证解析、前后台与子日志行为。
# ---------------------------------------------------------------------------
add_executable(launcher launcher/main.c)
target_include_directories(launcher PRIVATE "${PROJECT_SOURCE_DIR}/include/firmware/src")
target_compile_features(launcher PRIVATE c_std_11)
target_compile_options(launcher PRIVATE -Wall -Wextra -Wpedantic
    -ffunction-sections -fdata-sections)
if(CMAKE_CROSSCOMPILING)
    # 板端 ELF 必须链接 k230_rtos_app：它提供 RT-Smart 链接脚本（-T link.lds）
    # 与 -static。少了它就会链成动态 ELF（interpreter /lib/ld-musl-riscv64.so.1、
    # 入口不是 0x200000000），RT-Smart 用户态加载不了。host 构建不需要。
    target_link_libraries(launcher PRIVATE k230_rtos_app)
endif()
set_target_properties(launcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/launcher")

