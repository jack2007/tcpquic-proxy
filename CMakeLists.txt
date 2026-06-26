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

# Windows: skip XDP-for-Windows datapath; use socket raw path only.
if(WIN32)
    option(TCPQUIC_MSQUIC_DISABLE_XDP "Build msquic without Windows XDP datapath" ON)
else()
    set(TCPQUIC_MSQUIC_DISABLE_XDP OFF CACHE BOOL "Build msquic without Windows XDP datapath" FORCE)
endif()

# Always use bundled quictls; never Schannel/system TLS.
set(QUIC_TLS_LIB "quictls" CACHE STRING "TLS Library to use" FORCE)
set(QUIC_USE_SYSTEM_LIBCRYPTO OFF CACHE BOOL "Use vendored quictls crypto" FORCE)
if(TCPQUIC_MSQUIC_DISABLE_XDP)
    set(QUIC_DISABLE_XDP ON CACHE BOOL "Disable Windows XDP raw datapath at build time" FORCE)
endif()

option(TCPQUIC_USE_MIMALLOC "Statically link vendored mimalloc for relay buffers" ON)
set(TCPQUIC_ADDRESS_SANITIZER_ENABLED OFF)
set(TCPQUIC_SANITIZER_FLAGS
    "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS}")
if(TCPQUIC_SANITIZER_FLAGS MATCHES "(^|[ =,])-fsanitize=address([, ]|$)")
    set(TCPQUIC_ADDRESS_SANITIZER_ENABLED ON)
endif()
if(QUIC_ENABLE_ASAN OR QUIC_ENABLE_ALL_SANITIZERS)
    set(TCPQUIC_ADDRESS_SANITIZER_ENABLED ON)
endif()
if(TCPQUIC_ADDRESS_SANITIZER_ENABLED AND TCPQUIC_USE_MIMALLOC)
    message(STATUS "AddressSanitizer detected; disabling static mimalloc override")
    set(TCPQUIC_USE_MIMALLOC OFF CACHE BOOL "Statically link vendored mimalloc for relay buffers" FORCE)
endif()

if(DEFINED CACHE{TCPQUIC_MSQUIC_USE_MIMALLOC})
    get_property(TCPQUIC_MSQUIC_USE_MIMALLOC_CACHE_TYPE
        CACHE TCPQUIC_MSQUIC_USE_MIMALLOC PROPERTY TYPE)
    if(TCPQUIC_MSQUIC_USE_MIMALLOC_CACHE_TYPE STREQUAL "BOOL")
        string(TOUPPER "${TCPQUIC_MSQUIC_USE_MIMALLOC}" TCPQUIC_MSQUIC_USE_MIMALLOC_BOOL_VALUE)
        if(TCPQUIC_MSQUIC_USE_MIMALLOC_BOOL_VALUE MATCHES "^(1|ON|YES|TRUE|Y)$")
            set(TCPQUIC_MSQUIC_USE_MIMALLOC ON CACHE STRING
                "Use mimalloc in vendored MsQuic platform allocator: AUTO, ON, or OFF" FORCE)
        elseif(TCPQUIC_MSQUIC_USE_MIMALLOC_BOOL_VALUE MATCHES "^(0|OFF|NO|FALSE|N|IGNORE)$"
            OR TCPQUIC_MSQUIC_USE_MIMALLOC_BOOL_VALUE MATCHES "-NOTFOUND$")
            set(TCPQUIC_MSQUIC_USE_MIMALLOC OFF CACHE STRING
                "Use mimalloc in vendored MsQuic platform allocator: AUTO, ON, or OFF" FORCE)
        else()
            set(TCPQUIC_MSQUIC_USE_MIMALLOC "${TCPQUIC_MSQUIC_USE_MIMALLOC}" CACHE STRING
                "Use mimalloc in vendored MsQuic platform allocator: AUTO, ON, or OFF" FORCE)
        endif()
    else()
        set(TCPQUIC_MSQUIC_USE_MIMALLOC "${TCPQUIC_MSQUIC_USE_MIMALLOC}" CACHE STRING
            "Use mimalloc in vendored MsQuic platform allocator: AUTO, ON, or OFF" FORCE)
    endif()
else()
    set(TCPQUIC_MSQUIC_USE_MIMALLOC AUTO CACHE STRING
        "Use mimalloc in vendored MsQuic platform allocator: AUTO, ON, or OFF")
endif()
set_property(CACHE TCPQUIC_MSQUIC_USE_MIMALLOC PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${TCPQUIC_MSQUIC_USE_MIMALLOC}" TCPQUIC_MSQUIC_USE_MIMALLOC_MODE)

set(TCPQUIC_MSQUIC_MIMALLOC_ENABLED OFF)
if(TCPQUIC_MSQUIC_USE_MIMALLOC_MODE STREQUAL "AUTO")
    set(TCPQUIC_MSQUIC_MIMALLOC_ENABLED ${TCPQUIC_USE_MIMALLOC})
elseif(TCPQUIC_MSQUIC_USE_MIMALLOC_MODE STREQUAL "ON")
    if(NOT TCPQUIC_USE_MIMALLOC)
        message(FATAL_ERROR "TCPQUIC_MSQUIC_USE_MIMALLOC=ON requires TCPQUIC_USE_MIMALLOC=ON")
    endif()
    set(TCPQUIC_MSQUIC_MIMALLOC_ENABLED ON)
elseif(TCPQUIC_MSQUIC_USE_MIMALLOC_MODE STREQUAL "OFF")
    set(TCPQUIC_MSQUIC_MIMALLOC_ENABLED OFF)
else()
    message(FATAL_ERROR
        "TCPQUIC_MSQUIC_USE_MIMALLOC must be one of AUTO, ON, or OFF; got '${TCPQUIC_MSQUIC_USE_MIMALLOC}'")
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
    # Relay buffers use mi_malloc explicitly; global override breaks on macOS when
    # msquic/spdlog/STL mix allocators (see microsoft/mimalloc#1239).
    set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
    set(MI_OSX_INTERPOSE OFF CACHE BOOL "" FORCE)
    set(MI_OSX_ZONE OFF CACHE BOOL "" FORCE)
    set(TCPQUIC_MIMALLOC_EXTRA_CPPDEFS)
    if(MI_EXTRA_CPPDEFS)
        set(TCPQUIC_MIMALLOC_EXTRA_CPPDEFS ${MI_EXTRA_CPPDEFS})
        list(FILTER TCPQUIC_MIMALLOC_EXTRA_CPPDEFS EXCLUDE REGEX "^(OFF|MI_STAT(=.*)?)$")
    endif()
    # Admin memory diagnostics rely on requested-byte and allocation-count
    # counters, which mimalloc only maintains when MI_STAT is greater than 1.
    list(APPEND TCPQUIC_MIMALLOC_EXTRA_CPPDEFS MI_STAT=2)
    set(MI_EXTRA_CPPDEFS "${TCPQUIC_MIMALLOC_EXTRA_CPPDEFS}" CACHE STRING "" FORCE)
    add_subdirectory("${TCPQUIC_MIMALLOC_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/mimalloc")
    # MsQuic's static archive flattener records only static targets with an
    # explicit archive output directory. MSVC multi-config generators append
    # $<CONFIG> unless per-config output directories are set explicitly.
    set(_TCPQUIC_MIMALLOC_ARCHIVE_DIR "${CMAKE_BINARY_DIR}/mimalloc")
    set_target_properties(mimalloc-static PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${_TCPQUIC_MIMALLOC_ARCHIVE_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${_TCPQUIC_MIMALLOC_ARCHIVE_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${_TCPQUIC_MIMALLOC_ARCHIVE_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${_TCPQUIC_MIMALLOC_ARCHIVE_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${_TCPQUIC_MIMALLOC_ARCHIVE_DIR}")
endif()

if(TCPQUIC_MSQUIC_MIMALLOC_ENABLED)
    if(NOT TARGET mimalloc-static)
        message(FATAL_ERROR "TCPQUIC_MSQUIC_MIMALLOC_ENABLED requires the mimalloc-static target")
    endif()
endif()

add_subdirectory("${MSQUIC_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/msquic")

if(TCPQUIC_MSQUIC_MIMALLOC_ENABLED AND TARGET msquic_lib AND TARGET mimalloc-static)
    add_dependencies(msquic_lib mimalloc-static)
endif()

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
