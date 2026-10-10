add_library(dart_camera_core STATIC
    camera/config.c
    camera/exposure_plan.c
    camera/frame_lease.c)
target_include_directories(dart_camera_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_options(dart_camera_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dart_camera_core PUBLIC m)

option(DART_CAMERA_K230 "Build the real K230 camera ACL (cross compiler required)" OFF)
if(DART_CAMERA_K230)
    if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "riscv")
        message(FATAL_ERROR "DART_CAMERA_K230 requires a RISC-V cross toolchain")
    endif()
    set(K230_MPP_LIB_DIR "${K230_SDK_ROOT}/src/rtsmart/mpp/userapps/lib"
        CACHE PATH "K230 MPP static library directory")
    foreach(_library vicap vb sensor isp sys framework)
        if(NOT EXISTS "${K230_MPP_LIB_DIR}/lib${_library}.a")
            message(FATAL_ERROR "Missing K230 MPP library: lib${_library}.a")
        endif()
    endforeach()
    if(NOT EXISTS "${K230_LINK_SCRIPT}")
        message(FATAL_ERROR "Missing RT-Smart linker script: ${K230_LINK_SCRIPT}")
    endif()
    add_library(dart_camera STATIC camera/camera_k230.c)
    target_include_directories(dart_camera PUBLIC "${PROJECT_SOURCE_DIR}/include")
    target_compile_options(dart_camera PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(dart_camera PUBLIC dart_camera_core PRIVATE k230_rtos_app
        -Wl,--start-group
        "${K230_MPP_LIB_DIR}/libvicap.a"
        "${K230_MPP_LIB_DIR}/libvb.a"
        "${K230_MPP_LIB_DIR}/libsensor.a"
        "${K230_MPP_LIB_DIR}/libisp.a"
        "${K230_MPP_LIB_DIR}/libsys.a"
        "${K230_MPP_LIB_DIR}/libframework.a"
        -Wl,--end-group pthread m)
    add_executable(dart_camera_cpp_probe EXCLUDE_FROM_ALL
        "${PROJECT_SOURCE_DIR}/tests/camera_cpp_probe.cpp")
    target_link_libraries(dart_camera_cpp_probe PRIVATE dart_camera)
    target_compile_options(dart_camera_cpp_probe PRIVATE -Wall -Wextra -Wpedantic
        -fno-exceptions -fno-rtti)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(dart_camera_c_abi_test "${PROJECT_SOURCE_DIR}/tests/camera_c_abi_test.c")
    target_link_libraries(dart_camera_c_abi_test PRIVATE dart_camera_core)
    add_test(NAME dart_camera_c_abi_test COMMAND dart_camera_c_abi_test)
    add_executable(dart_camera_api_test "${PROJECT_SOURCE_DIR}/tests/camera_api_test.cpp")
    target_link_libraries(dart_camera_api_test PRIVATE dart_camera_core)
    target_include_directories(dart_camera_api_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/camera")
    target_compile_options(dart_camera_api_test PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions)
    add_test(NAME dart_camera_api_test COMMAND dart_camera_api_test)
    add_executable(dart_camera_adapter_test
        "${PROJECT_SOURCE_DIR}/tests/camera_adapter_test.cpp")
    target_include_directories(dart_camera_adapter_test PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_options(dart_camera_adapter_test PRIVATE -Wall -Wextra -Wpedantic
        -fno-exceptions -fno-rtti)
    add_test(NAME dart_camera_adapter_test COMMAND dart_camera_adapter_test)
endif()
