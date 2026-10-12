option(DART_CAMERA_K230 "Build the K230 camera data plane and C++ adapter" OFF)

# Keep the target helper strict: every enabled target must contain real sources.
# An interface fallback is retained only for optional targets that are disabled
# by configuration; no placeholder translation units are generated.
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
        message(STATUS "${target}: no sources enabled")
    endif()
    target_include_directories(${target} ${_scope}
        "${PROJECT_SOURCE_DIR}/include/firmware/app/include"
        "${PROJECT_SOURCE_DIR}/include/firmware/app/src")
    set(${target}_scope "${_scope}" PARENT_SCOPE)
endfunction()

_dart_camera_add_library(dart_camera_core c_api/config.c c_api/lease.c)
target_compile_features(dart_camera_core ${dart_camera_core_scope} c_std_11)

# Editor support: clangd can only use sources that have a compile command, and a
# source without one gets a flag-less fallback command in which every vendor
# header looks missing. The K230 data plane is only compiled by the cross
# preset, so when it is off the SDK include paths would never reach
# compile_commands.json. This target exists purely to emit those commands: it is
# EXCLUDE_FROM_ALL (never built, never linked, no artifact) and carries exactly
# the same include set as dart_camera_k230. The SDK paths are resolved by
# k230_rtos_app, so nothing is hardcoded here.
option(DART_CAMERA_CLANGD_COMMANDS
    "Emit compile commands for SDK-dependent sources so editors can parse them" ON)
if(DART_CAMERA_CLANGD_COMMANDS AND NOT DART_CAMERA_K230)
    add_library(dart_camera_k230_compile_commands OBJECT EXCLUDE_FROM_ALL
        k230/lifecycle.c k230/media.c k230/frame.c k230/exposure.c)
    target_link_libraries(dart_camera_k230_compile_commands PRIVATE k230_rtos_app)
    target_compile_features(dart_camera_k230_compile_commands PRIVATE c_std_11)
    target_compile_options(dart_camera_k230_compile_commands PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(dart_camera_k230_compile_commands PRIVATE
        "${PROJECT_SOURCE_DIR}/include/firmware/app/include"
        "${PROJECT_SOURCE_DIR}/include/firmware/app/src")
    # A header has no compile command of its own, so clangd borrows one from a
    # neighbouring file. Marking k230/internal.h as a compilable source gives it
    # the SDK include paths deterministically, instead of depending on which
    # neighbour clangd happens to pick.
    set_source_files_properties(k230/internal.h PROPERTIES LANGUAGE C)
    target_sources(dart_camera_k230_compile_commands PRIVATE k230/internal.h)
endif()

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
        k230/lifecycle.c k230/media.c k230/frame.c k230/exposure.c)
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

set(_acl_sources error.cpp)
if(DART_CAMERA_K230)
    list(APPEND _acl_sources k230/camera.cpp)
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
