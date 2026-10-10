option(DART_CAMERA_K230 "Build the K230 camera data plane and C++ adapter" OFF)

# T1 freezes targets before T2..T7 add implementations. Use interface targets
# until actual sources exist; never add empty C/C++ translation units. Re-run
# configuration after adding a planned source to include it in the build.
function(_dart_camera_add_library target)
    set(_sources)
    foreach(_source IN LISTS ARGN)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_source}")
            list(APPEND _sources "${_source}")
        endif()
    endforeach()
    if(_sources)
        add_library(${target} STATIC ${_sources})
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        set(_scope PUBLIC)
    else()
        add_library(${target} INTERFACE)
        set(_scope INTERFACE)
        message(STATUS "${target}: interface skeleton (implementation pending)")
    endif()
    target_include_directories(${target} ${_scope}
        "${PROJECT_SOURCE_DIR}/include/firmware/app/include"
        "${PROJECT_SOURCE_DIR}/include/firmware/app/src")
    set(${target}_scope "${_scope}" PARENT_SCOPE)
endfunction()

_dart_camera_add_library(dart_camera_core camera_config.c frame_lease.c)
target_compile_features(dart_camera_core ${dart_camera_core_scope} c_std_11)

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
    _dart_camera_add_library(dart_camera_k230
        camera.c camera_media.c camera_frame.c camera_exposure.c)
    target_link_libraries(dart_camera_k230 ${dart_camera_k230_scope} dart_camera_core)
    if(dart_camera_k230_scope STREQUAL "PUBLIC")
        set(_sdk_scope PRIVATE)
        target_link_libraries(dart_camera_k230 PRIVATE k230_rtos_app)
    else()
        set(_sdk_scope INTERFACE)
        target_link_libraries(dart_camera_k230 INTERFACE "$<LINK_ONLY:k230_rtos_app>")
    endif()
    target_link_libraries(dart_camera_k230 ${_sdk_scope}
        -Wl,--start-group
        "${K230_MPP_LIB_DIR}/libvicap.a"
        "${K230_MPP_LIB_DIR}/libvb.a"
        "${K230_MPP_LIB_DIR}/libsensor.a"
        "${K230_MPP_LIB_DIR}/libisp.a"
        "${K230_MPP_LIB_DIR}/libsys.a"
        "${K230_MPP_LIB_DIR}/libframework.a"
        -Wl,--end-group pthread m)
endif()

set(_acl_sources camera_error.cpp)
if(DART_CAMERA_K230)
    list(APPEND _acl_sources k230_camera.cpp)
endif()
_dart_camera_add_library(dart_camera_acl ${_acl_sources})
target_compile_features(dart_camera_acl ${dart_camera_acl_scope} cxx_std_23)
target_link_libraries(dart_camera_acl ${dart_camera_acl_scope} dart_camera_core)
if(DART_CAMERA_K230)
    target_link_libraries(dart_camera_acl ${dart_camera_acl_scope} dart_camera_k230)
endif()
if(dart_camera_acl_scope STREQUAL "PUBLIC")
    target_compile_options(dart_camera_acl PRIVATE -fno-exceptions -fno-rtti)
endif()
