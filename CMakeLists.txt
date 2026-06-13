cmake_minimum_required(VERSION 3.16)
project(tcpquic-proxy C CXX)

set(MSQUIC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/msquic" CACHE PATH "msquic source tree")

if(NOT EXISTS "${MSQUIC_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "msquic submodule missing. Run: git submodule update --init --recursive")
endif()

set(TCPQUIC_ZSTD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd" CACHE PATH "zstd source tree")

if(DEFINED ZSTD_SOURCE_DIR AND EXISTS "${ZSTD_SOURCE_DIR}/lib/zstd.h")
    set(TCPQUIC_ZSTD_SOURCE_DIR "${ZSTD_SOURCE_DIR}" CACHE PATH "zstd source tree" FORCE)
endif()

if(NOT EXISTS "${TCPQUIC_ZSTD_SOURCE_DIR}/lib/zstd.h")
    message(FATAL_ERROR "zstd submodule missing. Run: git submodule update --init --recursive")
endif()

set(QUIC_BUILD_TOOLS OFF CACHE BOOL "Build msquic tools" FORCE)
set(QUIC_ENABLE_LOGGING ON CACHE BOOL "" FORCE)

set(QUIC_TLS_LIB "quictls" CACHE STRING "TLS Library to use" FORCE)
if(WIN32)
    set(QUIC_OPENSSL_EXTRA_CONFIG_FLAGS "no-asm" CACHE STRING "Extra OpenSSL/QuicTLS configure flags" FORCE)
endif()

add_subdirectory("${MSQUIC_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/msquic")

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
add_subdirectory("${TCPQUIC_ZSTD_SOURCE_DIR}/build/cmake" "${CMAKE_BINARY_DIR}/zstd")

set(TCPQUIC_SPDLOG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog" CACHE PATH "spdlog source tree")
if(NOT EXISTS "${TCPQUIC_SPDLOG_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "spdlog submodule missing. Run: git submodule update --init --recursive")
endif()

set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${TCPQUIC_SPDLOG_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/spdlog")

# Align tool/test binaries with msquic convention: build/bin/Release/
set(TCPQUIC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<IF:$<CONFIG:Debug>,Debug,Release>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TCPQUIC_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${TCPQUIC_OUTPUT_DIR}")

add_subdirectory(src)
