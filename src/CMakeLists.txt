# ============================================================================
# 业务实现层（根目录 src/）：detection 与 recording。
#
# 两个模块的消费方式不同：
#   - detection：实现写在 .cpp 内部（类内定义 + 重复包含保护），因此对外只暴露
#     INTERFACE 目标；实现由消费者 #include "src/detection/roi.cpp" 引入，不能
#     再把这些实现编译成独立翻译单元（否则重复定义）。
#   - recording：recorder.cpp 自包含（声明与实现同体），由 dart_recording 单独
#     编译为静态库；消费者也可按 detection 的方式直接 #include 它。
# ============================================================================

# ---------------------------------------------------------------------------
# detection：检测 + ROI，纯 C++23，只依赖相机 ACL 的只读 FrameView
# ---------------------------------------------------------------------------
add_library(dart_detection INTERFACE)
target_compile_features(dart_detection INTERFACE cxx_std_23)
target_include_directories(dart_detection INTERFACE "${PROJECT_SOURCE_DIR}")
target_link_libraries(dart_detection INTERFACE dart_camera_acl)

# ---------------------------------------------------------------------------
# recording：灰度证据录像，单翻译单元静态库
# 租约、放行与 release 队列的约定见 src/recording/README.md
# 与 dart_camera_acl 一致启用 -fno-exceptions -fno-rtti：错误路径走返回码，
# 线程创建用 pthread_create（见 recorder.cpp 的 image_thread_entry）。
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(dart_recording STATIC recording/recorder.cpp)
add_library(dart_recorder ALIAS dart_recording)
target_compile_features(dart_recording PUBLIC cxx_std_23)
target_compile_options(dart_recording PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
target_include_directories(dart_recording
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include/firmware/app/src")
target_link_libraries(dart_recording PUBLIC dart_camera_acl Threads::Threads)
