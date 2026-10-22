# ===========================================================================
# include/ —— L3/L2 接口和实现库目标的唯一构建文件
#
# 约定：模块目录（camera/、spi/、imu/ …）只放源码和 README，不再各自持有
# CMakeLists.txt；所有库目标在这里集中声明，新增模块在下面追加一节即可。
# 本文件里的路径一律以 include/ 为基准，通过下面的模块目录变量拼接。
#
# 节的先后顺序有意义：spi/ 声明 DART_IMU_K230 并提供 dart_iim42652，imu/ 的
# C++ 适配器链接它，因此 spi 一节必须排在 imu 之前。
# ===========================================================================

set(_dart_include "${CMAKE_CURRENT_SOURCE_DIR}/firmware/include")
set(_dart_src "${CMAKE_CURRENT_SOURCE_DIR}/firmware/src")
set(_camera_dir "${_dart_src}/camera")
set(_spi_dir "${_dart_src}/spi")
set(_imu_dir "${_dart_src}/imu")

# ---------------------------------------------------------------------------
# camera —— C 数据面（C ABI 状态机）+ K230 数据面 + C++ 适配器（ICamera）
# ---------------------------------------------------------------------------
option(DART_CAMERA_K230 "Build the K230 camera data plane and C++ adapter" OFF)

# L3 frame value types only: no compiled camera code, SDK headers or link flags.
# L4 detectors consume this target; the composition root selects the camera ACL.
add_library(dart_camera_types INTERFACE)
target_compile_features(dart_camera_types INTERFACE cxx_std_23)
target_include_directories(dart_camera_types INTERFACE "${_dart_src}")

# Keep the target helper strict: every enabled target must contain real sources.
# An interface fallback is retained only for optional targets that are disabled
# by configuration; no placeholder translation units are generated.
function(_dart_camera_add_library target)
    set(_sources)
    foreach(_source IN LISTS ARGN)
        if(EXISTS "${_camera_dir}/${_source}")
            list(APPEND _sources "${_camera_dir}/${_source}")
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
        "${_dart_include}"
        "${_dart_src}")
    set(${target}_scope "${_scope}" PARENT_SCOPE)
endfunction()

_dart_camera_add_library(dart_camera_core c_api/config.c c_api/lease.c c_api/raw10.c)
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
        "${_camera_dir}/k230/lifecycle.c"
        "${_camera_dir}/k230/media.c"
        "${_camera_dir}/k230/frame.c"
        "${_camera_dir}/k230/mcm.c"
        "${_camera_dir}/k230/exposure.c")
    target_link_libraries(dart_camera_k230_compile_commands PRIVATE k230_rtos_app)
    target_compile_features(dart_camera_k230_compile_commands PRIVATE c_std_11)
    target_compile_options(dart_camera_k230_compile_commands PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(dart_camera_k230_compile_commands PRIVATE
        "${_dart_include}"
        "${_dart_src}")
    # A header has no compile command of its own, so clangd borrows one from a
    # neighbouring file. Marking k230/internal.h as a compilable source gives it
    # the SDK include paths deterministically, instead of depending on which
    # neighbour clangd happens to pick.
    set_source_files_properties("${_camera_dir}/k230/internal.h" PROPERTIES LANGUAGE C)
    target_sources(dart_camera_k230_compile_commands PRIVATE
        "${_camera_dir}/k230/internal.h")
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
    # The backend owns the audited private ABI. The standalone probe reads the
    # same manifest; changing either SDK library must re-run this check.
    set(_mcm_abi_path "${_camera_dir}/k230/mcm_abi.json")
    file(READ "${_mcm_abi_path}" _mcm_abi)
    string(JSON _checked_vicap_user GET "${_mcm_abi}" "userapps/lib/libvicap.a")
    string(JSON _checked_vicap_kernel GET "${_mcm_abi}" "kernel/lib/libvicap.a")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_mcm_abi_path}"
        "${K230_MPP_LIB_DIR}/libvicap.a"
        "${K230_SDK_ROOT}/src/rtsmart/mpp/kernel/lib/libvicap.a")
    file(SHA256 "${K230_MPP_LIB_DIR}/libvicap.a" _vicap_user_hash)
    file(SHA256 "${K230_SDK_ROOT}/src/rtsmart/mpp/kernel/lib/libvicap.a" _vicap_kernel_hash)
    if(NOT _vicap_user_hash STREQUAL _checked_vicap_user
       OR NOT _vicap_kernel_hash STREQUAL _checked_vicap_kernel)
        message(FATAL_ERROR "VICAP libraries changed; review the RAW10 MCM ioctl ABI before updating ${_mcm_abi_path}")
    endif()
    if(NOT EXISTS "${K230_LINK_SCRIPT}")
        message(FATAL_ERROR "Missing RT-Smart linker script: ${K230_LINK_SCRIPT}")
    endif()
    _dart_camera_add_library(dart_camera_k230
        k230/lifecycle.c k230/media.c k230/frame.c k230/exposure.c k230/mcm.c)
    target_link_options(dart_camera_k230 INTERFACE -Wl,--wrap=ioctl)
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

add_library(dart_camera_acl INTERFACE)
target_compile_features(dart_camera_acl INTERFACE cxx_std_23)
target_include_directories(dart_camera_acl INTERFACE "${_dart_include}" "${_dart_src}")
target_link_libraries(dart_camera_acl INTERFACE dart_camera_types dart_camera_core)
target_compile_options(dart_camera_acl INTERFACE
    -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
if(DART_CAMERA_K230)
    target_link_libraries(dart_camera_acl INTERFACE dart_camera_k230)
endif()
file(GLOB_RECURSE _camera_classes CONFIGURE_DEPENDS "${_camera_dir}/*.cpp")
add_library(dart_camera_checks OBJECT ${_camera_classes})
target_link_libraries(dart_camera_checks PRIVATE dart_camera_acl)

# ---------------------------------------------------------------------------
# spi —— 底层 C 封装：IIM-42652 SPI 硬件驱动
#
# iim42652.h 是稳定 C ABI（只有固定宽度整数和 POD），iim42652.c 是本仓库唯一
# 包含 K230 SDK 厂商头（drv_spi）的翻译单元，负责 SPI 总线
# 句柄、寄存器配置、原始值换算和厂商状态码翻译。本模块不引入 C++。
# ---------------------------------------------------------------------------
option(DART_IMU_K230 "Build the K230 IIM-42652 SPI backend" OFF)

if(DART_IMU_K230)
    if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "riscv")
        message(FATAL_ERROR "DART_IMU_K230 requires a RISC-V cross toolchain")
    endif()
    add_library(dart_iim42652 STATIC "${_spi_dir}/iim42652.c")
    # 驱动按 C 数据面约束编译：显式 gnu11，严格告警。
    target_compile_options(dart_iim42652 PRIVATE -Wall -Wextra -Wpedantic -std=gnu11)
    target_include_directories(dart_iim42652 PUBLIC "${_dart_src}")
    target_link_libraries(dart_iim42652 PRIVATE k230_rtos_app m)
endif()

# ---------------------------------------------------------------------------
# imu —— 设备抽象（IImu）、估计器与 K230 适配器
#
# 纯 C++，不含厂商头文件：
#   sample.cpp / interface.cpp   IMU 数据类与抽象接口
#   estimator.cpp               姿态/速度估计器，纯计算、无分配
#   k230/iim42652.cpp            C 驱动到 IImu 的适配器
# ---------------------------------------------------------------------------
add_library(dart_imu INTERFACE)
target_compile_features(dart_imu INTERFACE cxx_std_23)
target_compile_options(dart_imu INTERFACE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
target_include_directories(dart_imu INTERFACE "${_dart_src}")
file(GLOB_RECURSE _imu_classes CONFIGURE_DEPENDS "${_imu_dir}/*.cpp")
add_library(dart_imu_checks OBJECT ${_imu_classes})
target_link_libraries(dart_imu_checks PRIVATE dart_imu)

if(DART_IMU_K230)
    if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "riscv")
        message(FATAL_ERROR "DART_IMU_K230 requires a RISC-V cross toolchain")
    endif()

    # 板上整条 IMU 通路禁用异常/RTTI：估计器原先与适配器同处一个目标，
    # 交叉构建下同样受该约束，拆库后在这里保持。
    add_library(dart_imu_k230 INTERFACE)
    target_compile_features(dart_imu_k230 INTERFACE cxx_std_23)
    target_compile_options(dart_imu_k230 INTERFACE
        -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
    target_include_directories(dart_imu_k230 INTERFACE "${_dart_src}")
    target_link_libraries(dart_imu_k230 INTERFACE dart_imu)
    # k230_rtos_app 提供 SDK include 与 autoconf.h。
    target_link_libraries(dart_imu_k230 INTERFACE dart_iim42652 k230_rtos_app m)
endif()

# Editor support: spi/iim42652.c is only built by the cross preset, so with
# DART_IMU_K230 off it needs an editor-only compile command. C++ classes already
# receive compile commands from dart_imu_checks. A borrowed command lacks the SDK
# include paths nor -std=gnu11, so drv_spi.h, drv_spi_inst_t, SPI_HAL_MODE_3 and
# the bool literals all look undeclared in the editor while the real build is
# clean. This target exists purely to emit those commands: it is
# EXCLUDE_FROM_ALL (never built, never linked, no artifact) and carries the same
# include set and flags as dart_iim42652 / dart_imu_k230. SDK paths are resolved
# by k230_rtos_app, so nothing is hardcoded here.
option(DART_IMU_CLANGD_COMMANDS
    "Emit compile commands for SDK-dependent IMU sources so editors can parse them" ON)
if(DART_IMU_CLANGD_COMMANDS AND NOT DART_IMU_K230)
    add_library(dart_imu_k230_compile_commands OBJECT EXCLUDE_FROM_ALL
        "${_spi_dir}/iim42652.c")
    target_link_libraries(dart_imu_k230_compile_commands PRIVATE k230_rtos_app)
    target_compile_features(dart_imu_k230_compile_commands PRIVATE cxx_std_23)
    target_compile_options(dart_imu_k230_compile_commands PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
    target_include_directories(dart_imu_k230_compile_commands PRIVATE "${_dart_src}")
    # A header has no compile command of its own, so clangd borrows one from a
    # neighbouring file. Declaring the C driver header as a source gives it this
    # target's include set deterministically instead of depending on which
    # neighbour clangd happens to pick.
    set_source_files_properties("${_spi_dir}/iim42652.h" PROPERTIES LANGUAGE C)
    target_sources(dart_imu_k230_compile_commands PRIVATE
        "${_spi_dir}/iim42652.h")
endif()

# ---------------------------------------------------------------------------
# System adapters: process lifetime, logging and filesystem implementation.
# ---------------------------------------------------------------------------
add_library(dart_platform INTERFACE)
target_compile_features(dart_platform INTERFACE cxx_std_23)
target_compile_options(dart_platform INTERFACE
    -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
target_include_directories(dart_platform INTERFACE "${_dart_src}" "${PROJECT_SOURCE_DIR}")
file(GLOB_RECURSE _platform_classes CONFIGURE_DEPENDS
    "${_dart_src}/system/*.cpp" "${_dart_src}/logging/*.cpp" "${_dart_src}/storage/*.cpp")
add_library(dart_platform_checks OBJECT ${_platform_classes})
target_link_libraries(dart_platform_checks PRIVATE dart_platform dart_camera_acl)

# ---------------------------------------------------------------------------
# recording：灰度证据录像，按职责包含 .cpp
# 租约、放行与 release 队列的约定见 include/firmware/src/recorder/README.md
# 与 dart_camera_acl 一致启用 -fno-exceptions -fno-rtti：错误路径走返回码，
# 线程创建用 pthread_create（见 recorder.cpp 的 image_thread_entry）。
#
# 所有方法均内联；INTERFACE 目标只传播依赖，OBJECT 目标逐文件检查源码。
# ---------------------------------------------------------------------------
add_library(dart_recording INTERFACE)
add_library(dart_recorder ALIAS dart_recording)
target_compile_features(dart_recording INTERFACE cxx_std_23)
target_compile_options(dart_recording INTERFACE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
target_include_directories(dart_recording
    INTERFACE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include/firmware/src")
if(CMAKE_CROSSCOMPILING)
    set(DART_ZLIB_INCLUDE_DIR "${K230_SDK_ROOT}/src/rtsmart/libs/3rd-party/libwebsockets/libwebsockets/win32port/zlib"
        CACHE PATH "SDK zlib headers (stable zlib 1.x ABI)")
    set(DART_ZLIB_LIBRARY "${K230_SDK_ROOT}/src/rtsmart/libs/opencv/lib/opencv4/3rdparty/libzlib.a"
        CACHE FILEPATH "RT-Smart zlib static library")
    if(NOT EXISTS "${DART_ZLIB_INCLUDE_DIR}/zlib.h" OR NOT EXISTS "${DART_ZLIB_LIBRARY}")
        message(FATAL_ERROR "PNG recording needs DART_ZLIB_INCLUDE_DIR and DART_ZLIB_LIBRARY")
    endif()
    add_library(dart_zlib INTERFACE)
    target_include_directories(dart_zlib SYSTEM INTERFACE "${DART_ZLIB_INCLUDE_DIR}")
    target_link_libraries(dart_zlib INTERFACE "${DART_ZLIB_LIBRARY}")
else()
    find_package(ZLIB REQUIRED)
    add_library(dart_zlib ALIAS ZLIB::ZLIB)
endif()
target_link_libraries(dart_recording INTERFACE dart_camera_acl Threads::Threads dart_zlib)
file(GLOB_RECURSE _recording_classes CONFIGURE_DEPENDS "${_dart_src}/recorder/*.cpp")
add_library(dart_recording_checks OBJECT ${_recording_classes})
target_link_libraries(dart_recording_checks PRIVATE dart_recording)

# ---------------------------------------------------------------------------
# Board composition: concrete device factories and initialization stay below L4.
# dart_imu_logging is declared by runtime/ after this directory is configured.
# ---------------------------------------------------------------------------
add_library(dart_board INTERFACE)
target_link_libraries(dart_board INTERFACE dart_platform dart_camera_acl dart_imu_logging)
if(DART_IMU_K230)
    target_compile_definitions(dart_board INTERFACE DART_APP_IMU_K230=1)
    target_link_libraries(dart_board INTERFACE dart_imu_k230)
endif()
add_library(dart_board_checks OBJECT "${_dart_src}/board/k230.cpp")
target_link_libraries(dart_board_checks PRIVATE dart_board)
