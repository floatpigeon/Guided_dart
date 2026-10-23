cmake_minimum_required(VERSION 3.20)
project(dart_video_player LANGUAGES CXX)

# Standalone host project: never pulled into the board's CMake/deployment graph.
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "video_player is a host tool; configure it with a host compiler")
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_library(dart_video_player INTERFACE)
target_compile_features(dart_video_player INTERFACE cxx_std_23)
target_compile_options(dart_video_player INTERFACE -Wall -Wextra -Wpedantic)
target_link_libraries(dart_video_player INTERFACE Qt${QT_VERSION_MAJOR}::Widgets)

add_executable(video_player main.cpp)
target_link_libraries(video_player PRIVATE dart_video_player)

foreach(component recording_types recording_paths csv image_reader recording image_view player)
    add_library(video_player_${component}_check OBJECT ${component}.cpp)
    target_link_libraries(video_player_${component}_check PRIVATE dart_video_player)
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(video_player_test player_test.cpp)
    target_link_libraries(video_player_test PRIVATE dart_video_player)
    add_test(NAME video_player COMMAND video_player_test)
    set_tests_properties(video_player PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

install(TARGETS video_player RUNTIME DESTINATION bin)
