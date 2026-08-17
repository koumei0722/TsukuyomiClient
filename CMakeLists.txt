cmake_minimum_required(VERSION 3.20)

project(Tsukuyomi
    VERSION 2.1.0
    LANGUAGES CXX)

enable_language(ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

include(cmake/CPM.cmake)

CPMAddPackage(
    NAME minhook
    GITHUB_REPOSITORY TsudaKageyu/minhook
    GIT_TAG v1.3.4)

CPMAddPackage(
    NAME nlohmann_json
    GITHUB_REPOSITORY nlohmann/json
    GIT_TAG v3.11.3
    OPTIONS "JSON_BuildTests OFF")

CPMAddPackage(
    NAME libhat
    GITHUB_REPOSITORY BasedInc/libhat
    GIT_TAG v0.9.0)

file(GLOB_RECURSE TSUKUYOMI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.asm")

add_library(Tsukuyomi SHARED ${TSUKUYOMI_SOURCES})

target_include_directories(Tsukuyomi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(Tsukuyomi PRIVATE
    minhook
    nlohmann_json::nlohmann_json
    libhat::libhat
    d2d1
    dwrite
    dwmapi)

target_compile_options(Tsukuyomi PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    $<$<COMPILE_LANGUAGE:CXX>:/permissive->)

target_compile_definitions(Tsukuyomi PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    TSUKUYOMI_VERSION="${PROJECT_VERSION}")

set_target_properties(Tsukuyomi PROPERTIES
    PREFIX "")
