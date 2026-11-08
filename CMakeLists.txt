cmake_minimum_required(VERSION 3.20)
project(BroadwayCPU LANGUAGES CXX)

set(BROADWAYCPU_CORE_SOURCES
    branches.cpp
    cpu.cpp
    decoder.cpp
    disc_image.cpp
    exceptions.cpp
    exi.cpp
    executable_loader.cpp
    floating_point.cpp
    integer.cpp
    loads_stores.cpp
    memory.cpp
    paired_single.cpp
    registers.cpp
    system.cpp)
list(APPEND BROADWAYCPU_CORE_SOURCES video_frame.cpp gx.cpp wii_boot.cpp wii_firmware.cpp arm_elf.cpp)

add_library(broadwaycpu_core STATIC ${BROADWAYCPU_CORE_SOURCES})
target_compile_features(broadwaycpu_core PUBLIC cxx_std_20)
target_include_directories(broadwaycpu_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(broadwaycpu_core PUBLIC bcrypt)
endif()

if(MSVC)
    target_compile_options(broadwaycpu_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(broadwaycpu_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(broadwaycpu_tests tests.cpp)
target_link_libraries(broadwaycpu_tests PRIVATE broadwaycpu_core)
target_compile_features(broadwaycpu_tests PRIVATE cxx_std_20)

add_executable(gh3_startup_probe tools/gh3_startup_probe.cpp)
target_link_libraries(gh3_startup_probe PRIVATE broadwaycpu_core)
target_compile_features(gh3_startup_probe PRIVATE cxx_std_20)

add_executable(gh3_iso_probe tools/gh3_iso_probe.cpp)
target_link_libraries(gh3_iso_probe PRIVATE broadwaycpu_core)
target_compile_features(gh3_iso_probe PRIVATE cxx_std_20)

add_executable(wii_firmware_probe tools/wii_firmware_probe.cpp)
add_executable(starlet_trace tools/starlet_trace.cpp)
target_link_libraries(wii_firmware_probe PRIVATE broadwaycpu_core)
target_link_libraries(starlet_trace PRIVATE broadwaycpu_core)
target_compile_features(starlet_trace PRIVATE cxx_std_20)
target_compile_features(wii_firmware_probe PRIVATE cxx_std_20)

add_executable(gh3_dol_scan tools/gh3_dol_scan.cpp)
target_link_libraries(gh3_dol_scan PRIVATE broadwaycpu_core)
target_compile_features(gh3_dol_scan PRIVATE cxx_std_20)

enable_testing()
add_test(NAME broadwaycpu_core_tests COMMAND broadwaycpu_tests)

if(WIN32 AND MSVC)
    add_executable(BroadwayCPU WIN32 main.cpp video_output.cpp)
    target_link_libraries(BroadwayCPU PRIVATE broadwaycpu_core d2d1 comdlg32 comctl32 shell32 ole32)
    target_compile_features(BroadwayCPU PRIVATE cxx_std_20)
endif()
