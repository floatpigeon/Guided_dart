# ============================================================================
# 业务实现层（根目录 src/）：detection、recording 与被拉起的可执行入口。
#
# 目标放置约定（与 include/CMakeLists.txt 一致）：模块子目录只放源码与 README，
# 所有库/可执行目标集中在本文件声明，不再各自持有 CMakeLists.txt。
#
# 可执行文件有两类：
#   launcher     平台无关的合规工具：纯 POSIX C11（fork/execv/waitpid），不含
#                任何厂商头文件，host 与 K230 交叉构建都必须通过。它是板端
#                RT-Smart 的唯一开机自启入口，自身不含业务逻辑。
#   dart_vision  K230 板端 payload：单进程内完成"取帧 → 识别 → 录像"，是本仓库
#                唯一允许持有 VICAP/VB 的进程（v1 实测多实例抢 VICAP 必死）。
#                它依赖 K230 相机 ACL，因此只在 DART_CAMERA_K230=ON 时存在。
#
# 产物路径固定为 <build>/<target>/<target>（build-cross/launcher/launcher 等），
# 与 README/DEBUG_GUIDE 里写的部署路径一致，deploy 脚本按此查找。
# ============================================================================

# ---------------------------------------------------------------------------
# detection：检测 + ROI
#
# detector.cpp 与 roi.cpp 是**两个独立翻译单元**（各自 #include "detector.hpp"，
# 方法在类外定义、文件内没有重复包含保护）：
#   detector.cpp -> BlobWorkspace / LightDetector / ArmorDetector
#   roi.cpp      -> internal::LightTracker / internal::RoiPolicy / RoiDetector
# 因此这里必须把它们编译成真正的静态库；消费者只包含 detector.hpp 并链接
# dart_detection。之前把本模块声明为 INTERFACE（指望消费者 #include 两个 .cpp）
# 既不编译、也不类型检查这两个文件，任何应用都会在链接期才发现问题。
# ---------------------------------------------------------------------------
add_library(dart_detection STATIC detector/detector.cpp detector/roi.cpp)
target_compile_features(dart_detection PUBLIC cxx_std_23)
target_compile_options(dart_detection PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
target_include_directories(dart_detection PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(dart_detection PUBLIC dart_camera_acl)

# ---------------------------------------------------------------------------
# recording：灰度证据录像，单翻译单元静态库
# 租约、放行与 release 队列的约定见 src/recordor/README.md
# 与 dart_camera_acl 一致启用 -fno-exceptions -fno-rtti：错误路径走返回码，
# 线程创建用 pthread_create（见 recorder.cpp 的 image_thread_entry）。
#
# 该目标与"直接 #include recorder.cpp"是**两种互斥的消费方式**：recorder.cpp 把
# 声明与实现合并在一个带包含保护的翻译单元里（没有 recorder.hpp），dart_vision
# 采用 include 方式，因此不链接本目标；其他应用若链接本目标，就不要再 include。
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

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

# ---------------------------------------------------------------------------
# dart_vision：K230 板端"识别 + 录像"单进程 payload
#
# 单进程是硬约束：相机 ACL 内部自建并独占 dart_media/VICAP/VB，同一时刻只能有
# 一个消费者。清单里只放这一条（v1 的教训见 src/launcher/startup.list 注释）。
# ---------------------------------------------------------------------------
if(DART_CAMERA_K230)
    add_executable(dart_vision vision/main.cpp)
    target_compile_features(dart_vision PRIVATE cxx_std_23)
    target_compile_options(dart_vision PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
    target_include_directories(dart_vision PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include/firmware/app/src")
    # dart_detection 带出 dart_camera_acl/dart_camera_k230 与 SDK 头/link.lds。
    target_link_libraries(dart_vision PRIVATE dart_detection Threads::Threads)
    set_target_properties(dart_vision PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dart_vision")
else()
    message(STATUS "dart_vision: DART_CAMERA_K230=OFF，跳过板端 payload（需交叉构建）")
endif()
