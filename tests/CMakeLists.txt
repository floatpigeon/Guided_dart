if(CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(camera_raw10_test camera_raw10_test.c)
target_link_libraries(camera_raw10_test PRIVATE dart_camera_core)
target_compile_options(camera_raw10_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME camera_raw10 COMMAND camera_raw10_test)

foreach(_test recorder_session detector_behavior camera_contract camera_adapter imu_estimator imu_session argument_parser service_isolation binary_pipeline)
    add_executable(${_test}_test "${_test}_test.cpp")
    target_link_libraries(${_test}_test PRIVATE dart_detection dart_recording dart_imu dart_imu_logging dart_platform)
    target_compile_options(${_test}_test PRIVATE -Werror -UNDEBUG)
    add_test(NAME ${_test} COMMAND ${_test}_test)
endforeach()

target_link_options(imu_session_test PRIVATE -Wl,--wrap=fwrite)

add_executable(recorder_storage_test recorder_storage_test.cpp)
target_link_libraries(recorder_storage_test PRIVATE dart_recording)
target_compile_options(recorder_storage_test PRIVATE -Werror -UNDEBUG)
target_link_options(recorder_storage_test PRIVATE -Wl,--wrap=statvfs -Wl,--wrap=fsync)
add_test(NAME recorder_storage COMMAND recorder_storage_test)

find_package(PNG REQUIRED)
add_executable(recorder_png_test recorder_png_test.cpp)
target_link_libraries(recorder_png_test PRIVATE dart_recording PNG::PNG)
target_compile_options(recorder_png_test PRIVATE -Werror -UNDEBUG)
target_link_options(recorder_png_test PRIVATE -Wl,--wrap=fsync)
add_test(NAME recorder_png COMMAND recorder_png_test)

set(_spi_hal_include "${K230_SDK_ROOT}/src/rtsmart/libs/rtsmart_hal/drivers/spi")
if(EXISTS "${_spi_hal_include}/drv_spi.h")
    add_executable(imu_driver_test imu_driver_test.c
        "${PROJECT_SOURCE_DIR}/include/firmware/src/spi/iim42652.c")
    target_include_directories(imu_driver_test PRIVATE
        "${PROJECT_SOURCE_DIR}/include/firmware/src" "${_spi_hal_include}")
    target_compile_options(imu_driver_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_libraries(imu_driver_test PRIVATE m)
    target_link_options(imu_driver_test PRIVATE -Wl,--wrap=nanosleep -Wl,--wrap=clock_gettime)
    add_test(NAME imu_driver COMMAND imu_driver_test)
endif()

# Compile several consumers of overlapping C++ files and link them together.
add_library(dart_odr_consumer OBJECT odr_consumer.cpp)
target_link_libraries(dart_odr_consumer PRIVATE dart_detection dart_recording dart_imu)
target_sources(camera_contract_test PRIVATE $<TARGET_OBJECTS:dart_odr_consumer>)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME rtos_scripts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_rtos_scripts.py")
add_test(NAME cpp_structure
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_cpp_structure.py")
add_test(NAME launcher_restart
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/launcher_restart_test.sh" $<TARGET_FILE:launcher>)
