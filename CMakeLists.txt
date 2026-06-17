cmake_minimum_required(VERSION 3.20)
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

# Default: monolithic libmsquic.a (core + platform + quictls). Set ON for libmsquic.so.
option(TCPQUIC_MSQUIC_SHARED "Build msquic as a shared library (libmsquic.so)" OFF)
set(QUIC_BUILD_SHARED ${TCPQUIC_MSQUIC_SHARED} CACHE BOOL "Builds msquic as a dynamic library" FORCE)

set(QUIC_TLS_LIB "quictls" CACHE STRING "TLS Library to use" FORCE)

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

option(TCPQUIC_USE_MIMALLOC "Statically link vendored mimalloc for relay buffers" ON)
set(TCPQUIC_ADDRESS_SANITIZER_ENABLED OFF)
set(TCPQUIC_SANITIZER_FLAGS
    "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS}")
if(TCPQUIC_SANITIZER_FLAGS MATCHES "(^|[ =,])-fsanitize=address([, ]|$)")
    set(TCPQUIC_ADDRESS_SANITIZER_ENABLED ON)
endif()
if(TCPQUIC_ADDRESS_SANITIZER_ENABLED AND TCPQUIC_USE_MIMALLOC)
    message(STATUS "AddressSanitizer detected; disabling static mimalloc override")
    set(TCPQUIC_USE_MIMALLOC OFF CACHE BOOL "Statically link vendored mimalloc for relay buffers" FORCE)
endif()

set(TCPQUIC_MIMALLOC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mimalloc" CACHE PATH "mimalloc source tree")
if(TCPQUIC_USE_MIMALLOC AND NOT EXISTS "${TCPQUIC_MIMALLOC_SOURCE_DIR}/include/mimalloc.h")
    message(FATAL_ERROR "mimalloc submodule missing. Run: git submodule update --init --recursive third_party/mimalloc")
endif()

if(TCPQUIC_USE_MIMALLOC)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    add_subdirectory("${TCPQUIC_MIMALLOC_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/mimalloc")
endif()

# Align tool/test binaries with msquic convention: build/bin/Release/
set(TCPQUIC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<IF:$<CONFIG:Debug>,Debug,Release>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TCPQUIC_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${TCPQUIC_OUTPUT_DIR}")

add_subdirectory(src)
