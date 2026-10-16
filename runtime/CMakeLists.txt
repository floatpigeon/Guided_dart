# Concrete runtime services and process supervision, outside the L4 domain.
# Module subdirectories contain sources and README files; targets live here.

# ---------------------------------------------------------------------------
# recording：灰度证据录像，单翻译单元静态库
# 租约、放行与 release 队列的约定见 runtime/recordor/README.md
# 与 dart_camera_acl 一致启用 -fno-exceptions -fno-rtti：错误路径走返回码，
# 线程创建用 pthread_create（见 recorder.cpp 的 image_thread_entry）。
#
# 该目标与"直接 #include recorder.cpp"是**两种互斥的消费方式**：recorder.cpp 把
# 声明与实现合并在一个带包含保护的翻译单元里（没有 recorder.hpp），dart_app
# 采用 include 方式，因此不链接本目标；其他应用若链接本目标，就不要再 include。
# ---------------------------------------------------------------------------
add_library(dart_recording STATIC recordor/recorder.cpp)
add_library(dart_recorder ALIAS dart_recording)
target_compile_features(dart_recording PUBLIC cxx_std_23)
target_compile_options(dart_recording PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
target_include_directories(dart_recording
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include/firmware/app/src")
target_link_libraries(dart_recording PUBLIC dart_camera_acl Threads::Threads)

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

