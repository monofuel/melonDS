# Copyright 2026 monoMelon contributors.
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(monomelon_regressions LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_QT_SDL OFF CACHE BOOL "Build the desktop frontend" FORCE)
set(ENABLE_OGLRENDERER OFF CACHE BOOL "Build OpenGL rendering" FORCE)
set(ENABLE_GDBSTUB OFF CACHE BOOL "Build the GDB stub" FORCE)
set(ENABLE_LTO_RELEASE OFF CACHE BOOL "Use release LTO" FORCE)
set(ENABLE_LTO OFF CACHE BOOL "Use LTO" FORCE)

get_filename_component(engine "../.." ABSOLUTE)
add_subdirectory("${engine}" engine)
get_directory_property(engine_architecture DIRECTORY "${engine}" DEFINITION ARCHITECTURE)
get_directory_property(engine_jit DIRECTORY "${engine}" DEFINITION ENABLE_JIT)
find_package(Threads REQUIRED)
enable_testing()

set(fixtures Scheduler Snapshots Bands3D)
if(engine_jit AND engine_architecture STREQUAL "ARM64")
    list(APPEND fixtures Dispatch)
endif()

foreach(fixture IN LISTS fixtures)
    add_executable(test-${fixture} Test${fixture}.cpp Bands3DPlatform.cpp)
    target_link_libraries(test-${fixture} PRIVATE core Threads::Threads)
    target_compile_definitions(test-${fixture} PRIVATE ARCHITECTURE_${engine_architecture}=1)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Test assertions must remain active when the engine uses release flags.
        target_compile_options(test-${fixture} PRIVATE -UNDEBUG -Wno-invalid-offsetof)
    endif()
    if(fixture STREQUAL "Bands3D")
        add_test(NAME ${fixture} COMMAND test-${fixture} 120)
    else()
        add_test(NAME ${fixture} COMMAND test-${fixture})
    endif()
endforeach()
