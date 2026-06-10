cmake_minimum_required(VERSION 3.16)
project(tcpquic-proxy C CXX)

set(MSQUIC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/msquic" CACHE PATH "msquic source tree")

if(NOT EXISTS "${MSQUIC_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "msquic submodule missing. Run: git submodule update --init --recursive")
endif()

set(LZ4_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4" CACHE PATH "lz4 source tree")
set(ZSTD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd" CACHE PATH "zstd source tree")

if(NOT EXISTS "${LZ4_SOURCE_DIR}/lib/lz4.h")
    message(FATAL_ERROR "lz4 submodule missing. Run: git submodule update --init --recursive")
endif()

if(NOT EXISTS "${ZSTD_SOURCE_DIR}/lib/zstd.h")
    message(FATAL_ERROR "zstd submodule missing. Run: git submodule update --init --recursive")
endif()

set(QUIC_BUILD_TOOLS OFF CACHE BOOL "Build msquic tools" FORCE)
set(QUIC_ENABLE_LOGGING ON CACHE BOOL "" FORCE)

# Optional: -DTCPQUIC_WINDOWS_TLS=quictls with Strawberry Perl enables PEM CA files without
# importing --quic-ca into the Windows trust store. Default schannel uses CryptoAPI PFX loading.
set(TCPQUIC_WINDOWS_TLS "schannel" CACHE STRING "Windows msquic TLS backend: schannel or quictls")
if(WIN32)
    if(TCPQUIC_WINDOWS_TLS STREQUAL "quictls")
        set(QUIC_TLS_LIB "quictls" CACHE STRING "TLS Library to use" FORCE)
        set(QUIC_OPENSSL_EXTRA_CONFIG_FLAGS "no-asm" CACHE STRING "Extra OpenSSL/QuicTLS configure flags" FORCE)
    else()
        set(QUIC_TLS_LIB "schannel" CACHE STRING "TLS Library to use" FORCE)
    endif()
endif()

add_subdirectory("${MSQUIC_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/msquic")

set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory("${LZ4_SOURCE_DIR}/build/cmake" "${CMAKE_BINARY_DIR}/lz4")

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
add_subdirectory("${ZSTD_SOURCE_DIR}/build/cmake" "${CMAKE_BINARY_DIR}/zstd")

# Align tool/test binaries with msquic convention: build/bin/Release/
set(TCPQUIC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<IF:$<CONFIG:Debug>,Debug,Release>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TCPQUIC_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${TCPQUIC_OUTPUT_DIR}")

add_subdirectory(src)
