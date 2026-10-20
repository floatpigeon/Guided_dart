# L4 modules call lower-level libraries from include/.
# Functional targets keep platform and device dependencies separate from algorithms.

# Cohesive files are included as guarded .cpp files; the interface target propagates
# requirements. Compile every file independently to check its dependencies.
add_library(dart_detection INTERFACE)
target_compile_features(dart_detection INTERFACE cxx_std_23)
target_compile_options(dart_detection INTERFACE
    -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
target_include_directories(dart_detection INTERFACE "${PROJECT_SOURCE_DIR}")
target_link_libraries(dart_detection INTERFACE dart_camera_types)

file(GLOB_RECURSE _detection_classes CONFIGURE_DEPENDS "detector/*.cpp")
add_library(dart_detection_checks OBJECT ${_detection_classes})
target_link_libraries(dart_detection_checks PRIVATE dart_detection)

# dart_app: one process owns VICAP/VB and connects devices, domain and runtime.
if(DART_CAMERA_K230)
    add_executable(dart_app dart_entry.cpp)
    target_compile_features(dart_app PRIVATE cxx_std_23)
    target_compile_options(dart_app PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
    target_include_directories(dart_app PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include/firmware/src")
    target_link_libraries(dart_app PRIVATE
        dart_detection dart_recording dart_imu_logging dart_platform dart_board Threads::Threads)
    set_target_properties(dart_app PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dart_app")
else()
    message(STATUS "dart_app: DART_CAMERA_K230=OFF，跳过板端应用（需交叉构建）")
endif()

# Independently compile includable application files on both host and board.
# The process entry is built only by the executable target above.
file(GLOB_RECURSE _application_classes CONFIGURE_DEPENDS
    "startup/*.cpp" "vision/*.cpp" "recording/*.cpp" "diagnostics/*.cpp")
add_library(dart_application_checks OBJECT ${_application_classes})
target_link_libraries(dart_application_checks PRIVATE dart_detection dart_recording dart_imu_logging dart_platform)
