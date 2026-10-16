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
