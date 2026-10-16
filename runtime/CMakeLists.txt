# Concrete runtime services and process supervision, outside the L4 domain.
# Module subdirectories contain sources and README files; targets live here.

# ---------------------------------------------------------------------------
# recording：灰度证据录像，按职责包含 .cpp
# 租约、放行与 release 队列的约定见 runtime/recordor/README.md
# 与 dart_camera_acl 一致启用 -fno-exceptions -fno-rtti：错误路径走返回码，
# 线程创建用 pthread_create（见 recorder.cpp 的 image_thread_entry）。
#
# 所有方法均内联；INTERFACE 目标只传播依赖，OBJECT 目标逐文件检查源码。
# ---------------------------------------------------------------------------
add_library(dart_recording INTERFACE)
add_library(dart_recorder ALIAS dart_recording)
target_compile_features(dart_recording INTERFACE cxx_std_23)
target_compile_options(dart_recording INTERFACE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
target_include_directories(dart_recording
    INTERFACE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include/firmware/app/src")
target_link_libraries(dart_recording INTERFACE dart_camera_acl Threads::Threads)
file(GLOB_RECURSE _recording_classes CONFIGURE_DEPENDS "recordor/*.cpp")
add_library(dart_recording_checks OBJECT ${_recording_classes})
target_link_libraries(dart_recording_checks PRIVATE dart_recording)

# ---------------------------------------------------------------------------
# launcher：开机自启动器（平台无关）
#
# 只做"读清单 → fork/execv 拉起 → 监督回收"，不认识相机、录像或任何 SDK。
# host 也能构建与运行，便于在电脑上用假清单验证解析、前后台与子日志行为。
# ---------------------------------------------------------------------------
add_executable(launcher launcher/main.c)
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

