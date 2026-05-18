cmake_minimum_required(VERSION 3.23)
project(AutoResolution LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CONFIGURATION_TYPES Debug;Release)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(RESHADE_SUFFIX 64)
else()
    set(RESHADE_SUFFIX 32)
endif()

add_library(AutoResolution SHARED auto_resolution.cpp)

set_target_properties(AutoResolution PROPERTIES
    PREFIX ""
    SUFFIX "${RESHADE_SUFFIX}.addon"
)

target_include_directories(AutoResolution PRIVATE include)

if(MSVC)
    target_compile_options(AutoResolution PRIVATE
        /utf-8
        /Zc:char8_t-
        $<$<CONFIG:Release>:/O2 /GS->
    )
endif()
