if(CMAKE_CROSSCOMPILING)
    return()
endif()

foreach(_test recorder_session detector_behavior camera_contract camera_adapter imu_estimator argument_parser)
    add_executable(${_test}_test "${_test}_test.cpp")
    target_link_libraries(${_test}_test PRIVATE dart_detection dart_recording dart_imu)
    target_compile_options(${_test}_test PRIVATE -Werror -UNDEBUG)
    add_test(NAME ${_test} COMMAND ${_test}_test)
endforeach()

# Compile several consumers of overlapping C++ files and link them together.
add_library(dart_odr_consumer OBJECT odr_consumer.cpp)
target_link_libraries(dart_odr_consumer PRIVATE dart_detection dart_recording dart_imu)
target_sources(camera_contract_test PRIVATE $<TARGET_OBJECTS:dart_odr_consumer>)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME cpp_structure
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_cpp_structure.py")
add_test(NAME launcher_restart
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/launcher_restart_test.sh" $<TARGET_FILE:launcher>)
