cmake_minimum_required(VERSION 3.20)

project(Sirenhead VERSION 1.0.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Sirenhead requires MSVC")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Sirenhead is an x32dbg plugin; configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

FetchContent_Declare(
    x64dbg
    URL "https://downloads.sourceforge.net/project/x64dbg/snapshots/snapshot_2023-06-10_18-05.zip"
    URL_HASH SHA1=04468bd61fb36d6b10d17f342f03ef12f5b2ce62
)
FetchContent_MakeAvailable(x64dbg)

file(GLOB_RECURSE X64DBG_SDK_LIBS
    "${x64dbg_SOURCE_DIR}/pluginsdk/*_x86.lib"
    "${x64dbg_SOURCE_DIR}/pluginsdk/x32*.lib"
)

add_library(Sirenhead SHARED
    src/fairplaykd.h
    src/plugin.cpp
    src/plugin.h
    src/pluginmain.cpp
    src/pluginmain.h
)

target_include_directories(Sirenhead PRIVATE "${x64dbg_SOURCE_DIR}")
target_link_libraries(Sirenhead PRIVATE ${X64DBG_SDK_LIBS})
target_compile_definitions(Sirenhead PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(Sirenhead PRIVATE /W4 /permissive- /utf-8)

set_target_properties(Sirenhead PROPERTIES
    PREFIX ""
    SUFFIX ".dp32"
    OUTPUT_NAME "Sirenhead"
)
