# Application composition root. This is where concrete device and runtime
# implementations are connected to the L4 domain.

# ---------------------------------------------------------------------------
# dart_app：K230 板端业务应用（main 入口 + 应用编排）
#
# 单进程是硬约束：相机 ACL 内部自建并独占 dart_media/VICAP/VB，同一时刻只能有
# 一个消费者。清单里只放这一条（v1 的教训见 runtime/launcher/startup.list 注释）。
# ---------------------------------------------------------------------------
if(DART_CAMERA_K230)
    add_executable(dart_app dart_entry.cpp)
    target_compile_features(dart_app PRIVATE cxx_std_23)
    target_compile_options(dart_app PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
    target_include_directories(dart_app PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include/firmware/app/src")
    # L4 detection only depends on frame types. Select the concrete camera here;
    # its ACL supplies the K230 backend, SDK dependencies and RT-Smart link flags.
    target_link_libraries(dart_app PRIVATE dart_detection dart_recording dart_imu_logging Threads::Threads)
    if(DART_IMU_K230)
        target_compile_definitions(dart_app PRIVATE DART_APP_IMU_K230=1)
        target_link_libraries(dart_app PRIVATE dart_imu_k230)
    endif()
    set_target_properties(dart_app PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dart_app")
else()
    message(STATUS "dart_app: DART_CAMERA_K230=OFF，跳过板端应用（需交叉构建）")
endif()

# Check each includable application file, including on the host. No device
# functions are executed or linked by this object-only target.
file(GLOB_RECURSE _application_classes CONFIGURE_DEPENDS "*.cpp")
list(REMOVE_ITEM _application_classes "${CMAKE_CURRENT_SOURCE_DIR}/dart_entry.cpp")
add_library(dart_application_checks OBJECT ${_application_classes})
target_link_libraries(dart_application_checks PRIVATE dart_detection dart_recording dart_imu_logging)
if(DART_IMU_K230)
    target_compile_definitions(dart_application_checks PRIVATE DART_APP_IMU_K230=1)
    target_link_libraries(dart_application_checks PRIVATE dart_imu_k230)
endif()
