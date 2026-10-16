# L4 domain algorithms and policies. Only public L3 types/interfaces may be
# dependencies here; concrete devices, runtime services and executables are
# declared under include/, runtime/ and app/.

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
target_link_libraries(dart_detection PUBLIC dart_camera_types)
