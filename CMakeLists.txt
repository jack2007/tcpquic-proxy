cmake_minimum_required(VERSION 3.20)
project(tcpquic-proxy C CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No build type selected; defaulting to RelWithDebInfo")
endif()

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

set(TCPQUIC_CPP_HTTPLIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib" CACHE PATH "cpp-httplib source tree")
if(NOT EXISTS "${TCPQUIC_CPP_HTTPLIB_SOURCE_DIR}/httplib.h")
    message(FATAL_ERROR "cpp-httplib submodule missing. Run: git submodule update --init --recursive third_party/cpp-httplib")
endif()

set(TCPQUIC_LIBUV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libuv" CACHE PATH "libuv source tree")
if(NOT EXISTS "${TCPQUIC_LIBUV_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "libuv submodule missing. Run: git submodule update --init --recursive third_party/libuv")
endif()

set(QUIC_BUILD_TOOLS OFF CACHE BOOL "Build msquic tools" FORCE)
set(QUIC_ENABLE_LOGGING ON CACHE BOOL "" FORCE)

# Default: monolithic libmsquic.a (core + platform + quictls). Set ON for libmsquic.so.
option(TCPQUIC_MSQUIC_SHARED "Build msquic as a shared library (libmsquic.so)" OFF)
set(QUIC_BUILD_SHARED ${TCPQUIC_MSQUIC_SHARED} CACHE BOOL "Builds msquic as a dynamic library" FORCE)

# Windows: skip XDP-for-Windows datapath; use socket raw path only.
if(WIN32)
    option(TCPQUIC_WINDOWS_TARGET_WIN7 "Build for Windows 7 through 11 (subsystem 6.01, API 0x0601)" ON)
else()
    set(TCPQUIC_WINDOWS_TARGET_WIN7 OFF CACHE BOOL "Build for Windows 7 through 11" FORCE)
endif()

if(WIN32)
    option(TCPQUIC_MSQUIC_DISABLE_XDP "Build msquic without Windows XDP datapath" ON)
else()
    set(TCPQUIC_MSQUIC_DISABLE_XDP OFF CACHE BOOL "Build msquic without Windows XDP datapath" FORCE)
endif()

if(WIN32 AND TCPQUIC_WINDOWS_TARGET_WIN7)
    message(STATUS "Windows target: Win7+ (_WIN32_WINNT=0x0601); raypx2 subsystem 6.01")
    add_compile_definitions(_WIN32_WINNT=0x0601 NTDDI_VERSION=0x06010000)
endif()

# Always use bundled quictls; never Schannel/system TLS.
set(QUIC_TLS_LIB "quictls" CACHE STRING "TLS Library to use" FORCE)
set(QUIC_USE_SYSTEM_LIBCRYPTO OFF CACHE BOOL "Use vendored quictls crypto" FORCE)
if(TCPQUIC_MSQUIC_DISABLE_XDP)
    set(QUIC_DISABLE_XDP ON CACHE BOOL "Disable Windows XDP raw datapath at build time" FORCE)
endif()

option(TCPQUIC_USE_MIMALLOC "Statically link vendored mimalloc for relay buffers" ON)

set(TCPQUIC_ENABLE_CRASHPAD AUTO CACHE STRING "Enable Crashpad crash dumps: AUTO, ON, or OFF")
set_property(CACHE TCPQUIC_ENABLE_CRASHPAD PROPERTY STRINGS AUTO ON OFF)
option(TCPQUIC_CRASHPAD_AUTO_BUILD
    "Build vendored Crashpad with GN/Ninja when required artifacts are missing"
    ON)
set(TCPQUIC_CRASHPAD_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crashpad"
    CACHE PATH "Crashpad source tree")
if(NOT EXISTS "${TCPQUIC_CRASHPAD_SOURCE_DIR}/client/crashpad_client.h" AND
   EXISTS "${TCPQUIC_CRASHPAD_SOURCE_DIR}/crashpad/client/crashpad_client.h")
    set(TCPQUIC_CRASHPAD_SOURCE_DIR
        "${TCPQUIC_CRASHPAD_SOURCE_DIR}/crashpad"
        CACHE PATH "Crashpad source tree" FORCE)
endif()
set(TCPQUIC_CRASHPAD_BUILD_DIR
    "${TCPQUIC_CRASHPAD_SOURCE_DIR}/out/Default"
    CACHE PATH "Crashpad GN/Ninja build output directory")

set(TCPQUIC_HAS_CRASHPAD OFF)
string(TOUPPER "${TCPQUIC_ENABLE_CRASHPAD}" TCPQUIC_ENABLE_CRASHPAD_MODE)
if(NOT TCPQUIC_ENABLE_CRASHPAD_MODE MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "TCPQUIC_ENABLE_CRASHPAD must be AUTO, ON, or OFF")
endif()

if(NOT TCPQUIC_ENABLE_CRASHPAD_MODE STREQUAL "OFF")
    set(TCPQUIC_CRASHPAD_CLIENT_HEADER "${TCPQUIC_CRASHPAD_SOURCE_DIR}/client/crashpad_client.h")
    set(TCPQUIC_CRASHPAD_MINI_CHROMIUM_DIR
        "${TCPQUIC_CRASHPAD_SOURCE_DIR}/third_party/mini_chromium/mini_chromium")
    set(TCPQUIC_CRASHPAD_MINI_CHROMIUM_HEADER
        "${TCPQUIC_CRASHPAD_MINI_CHROMIUM_DIR}/base/files/file_path.h")
    if(WIN32)
        set(TCPQUIC_CRASHPAD_HANDLER_NAME crashpad_handler.exe)
    else()
        set(TCPQUIC_CRASHPAD_HANDLER_NAME crashpad_handler)
    endif()
    set(TCPQUIC_CRASHPAD_HANDLER
        "${TCPQUIC_CRASHPAD_BUILD_DIR}/${TCPQUIC_CRASHPAD_HANDLER_NAME}")

    function(tcpquic_find_crashpad_artifacts)
        unset(TCPQUIC_CRASHPAD_CLIENT_LIBRARY CACHE)
        unset(TCPQUIC_CRASHPAD_UTIL_LIBRARY CACHE)
        unset(TCPQUIC_CRASHPAD_BASE_LIBRARY CACHE)
        unset(TCPQUIC_CRASHPAD_COMPAT_LIBRARY CACHE)
        unset(TCPQUIC_CRASHPAD_MIG_OUTPUT_LIBRARY CACHE)
        find_library(TCPQUIC_CRASHPAD_CLIENT_LIBRARY
            NAMES client libclient
            PATHS "${TCPQUIC_CRASHPAD_BUILD_DIR}/obj/client"
            NO_DEFAULT_PATH)
        find_library(TCPQUIC_CRASHPAD_UTIL_LIBRARY
            NAMES util libutil
            PATHS "${TCPQUIC_CRASHPAD_BUILD_DIR}/obj/util"
            NO_DEFAULT_PATH)
        find_library(TCPQUIC_CRASHPAD_BASE_LIBRARY
            NAMES base libbase
            PATHS "${TCPQUIC_CRASHPAD_BUILD_DIR}/obj/third_party/mini_chromium/mini_chromium/base"
            NO_DEFAULT_PATH)
        if(UNIX AND NOT APPLE)
            find_library(TCPQUIC_CRASHPAD_COMPAT_LIBRARY
                NAMES compat libcompat
                PATHS "${TCPQUIC_CRASHPAD_BUILD_DIR}/obj/compat"
                NO_DEFAULT_PATH)
        endif()
        if(APPLE)
            find_library(TCPQUIC_CRASHPAD_MIG_OUTPUT_LIBRARY
                NAMES mig_output libmig_output
                PATHS "${TCPQUIC_CRASHPAD_BUILD_DIR}/obj/util"
                NO_DEFAULT_PATH)
        endif()
    endfunction()

    function(tcpquic_crashpad_artifacts_ready out_var)
        if(EXISTS "${TCPQUIC_CRASHPAD_CLIENT_HEADER}" AND
           EXISTS "${TCPQUIC_CRASHPAD_MINI_CHROMIUM_HEADER}" AND
           EXISTS "${TCPQUIC_CRASHPAD_HANDLER}" AND
           TCPQUIC_CRASHPAD_CLIENT_LIBRARY AND
           TCPQUIC_CRASHPAD_UTIL_LIBRARY AND
           TCPQUIC_CRASHPAD_BASE_LIBRARY AND
           (NOT UNIX OR APPLE OR TCPQUIC_CRASHPAD_COMPAT_LIBRARY) AND
           (NOT APPLE OR TCPQUIC_CRASHPAD_MIG_OUTPUT_LIBRARY))
            set(${out_var} ON PARENT_SCOPE)
        else()
            set(${out_var} OFF PARENT_SCOPE)
        endif()
    endfunction()

    tcpquic_find_crashpad_artifacts()
    tcpquic_crashpad_artifacts_ready(TCPQUIC_CRASHPAD_READY)

    if(NOT TCPQUIC_CRASHPAD_READY AND TCPQUIC_CRASHPAD_AUTO_BUILD)
        find_program(TCPQUIC_GN_EXECUTABLE gn)
        find_program(TCPQUIC_NINJA_EXECUTABLE NAMES ninja ninja-build)
        set(TCPQUIC_CRASHPAD_LSS_DIR
            "${TCPQUIC_CRASHPAD_SOURCE_DIR}/third_party/lss/lss")
        set(TCPQUIC_CRASHPAD_ZLIB_DIR
            "${TCPQUIC_CRASHPAD_SOURCE_DIR}/third_party/zlib/zlib")
        set(TCPQUIC_CRASHPAD_VENDOR_READY ON)
        if(NOT EXISTS "${TCPQUIC_CRASHPAD_CLIENT_HEADER}" OR
           NOT EXISTS "${TCPQUIC_CRASHPAD_MINI_CHROMIUM_HEADER}" OR
           NOT EXISTS "${TCPQUIC_CRASHPAD_ZLIB_DIR}/zlib.h")
            set(TCPQUIC_CRASHPAD_VENDOR_READY OFF)
        endif()
        if(UNIX AND NOT APPLE AND
           NOT EXISTS "${TCPQUIC_CRASHPAD_LSS_DIR}/linux_syscall_support.h")
            set(TCPQUIC_CRASHPAD_VENDOR_READY OFF)
        endif()

        if(TCPQUIC_GN_EXECUTABLE AND TCPQUIC_NINJA_EXECUTABLE AND
           TCPQUIC_CRASHPAD_VENDOR_READY)
            set(TCPQUIC_CRASHPAD_GN_ARGS
                "is_debug=false crashpad_http_transport_impl=\"socket\"")
            set(TCPQUIC_CRASHPAD_BUILD_TARGETS
                crashpad_handler
                client:client
                util:util
                third_party/mini_chromium:base)
            if(APPLE)
                list(APPEND TCPQUIC_CRASHPAD_BUILD_TARGETS util:mig_output)
            endif()
            message(STATUS "Building vendored Crashpad: ${TCPQUIC_CRASHPAD_BUILD_DIR}")
            execute_process(
                COMMAND "${TCPQUIC_GN_EXECUTABLE}" gen "${TCPQUIC_CRASHPAD_BUILD_DIR}"
                    "--root=${TCPQUIC_CRASHPAD_SOURCE_DIR}"
                    "--args=${TCPQUIC_CRASHPAD_GN_ARGS}"
                RESULT_VARIABLE TCPQUIC_CRASHPAD_GN_RESULT)
            if(NOT TCPQUIC_CRASHPAD_GN_RESULT EQUAL 0)
                if(TCPQUIC_ENABLE_CRASHPAD_MODE STREQUAL "ON")
                    message(FATAL_ERROR "Crashpad GN generation failed")
                endif()
            else()
                execute_process(
                    COMMAND "${TCPQUIC_NINJA_EXECUTABLE}" -C "${TCPQUIC_CRASHPAD_BUILD_DIR}"
                        ${TCPQUIC_CRASHPAD_BUILD_TARGETS}
                    RESULT_VARIABLE TCPQUIC_CRASHPAD_NINJA_RESULT)
                if(NOT TCPQUIC_CRASHPAD_NINJA_RESULT EQUAL 0)
                    if(TCPQUIC_ENABLE_CRASHPAD_MODE STREQUAL "ON")
                        message(FATAL_ERROR "Crashpad Ninja build failed")
                    endif()
                endif()
            endif()
            tcpquic_find_crashpad_artifacts()
            tcpquic_crashpad_artifacts_ready(TCPQUIC_CRASHPAD_READY)
        elseif(TCPQUIC_ENABLE_CRASHPAD_MODE STREQUAL "ON")
            message(FATAL_ERROR
                "Crashpad auto-build requires gn, ninja, vendored Crashpad DEPS, "
                "and mini_chromium. Ensure submodules are initialized from the project fork.")
        endif()
    endif()

    if(TCPQUIC_CRASHPAD_READY)
        set(TCPQUIC_HAS_CRASHPAD ON)
        add_library(tcpquic_crashpad_client INTERFACE)
        target_include_directories(tcpquic_crashpad_client INTERFACE
            "${TCPQUIC_CRASHPAD_SOURCE_DIR}"
            "${TCPQUIC_CRASHPAD_MINI_CHROMIUM_DIR}")
        target_link_libraries(tcpquic_crashpad_client INTERFACE
            "${TCPQUIC_CRASHPAD_CLIENT_LIBRARY}"
            "${TCPQUIC_CRASHPAD_UTIL_LIBRARY}"
            "${TCPQUIC_CRASHPAD_BASE_LIBRARY}")
        if(UNIX AND NOT APPLE)
            target_link_libraries(tcpquic_crashpad_client INTERFACE
                "${TCPQUIC_CRASHPAD_COMPAT_LIBRARY}")
        endif()
        if(APPLE)
            target_link_libraries(tcpquic_crashpad_client INTERFACE
                "${TCPQUIC_CRASHPAD_MIG_OUTPUT_LIBRARY}")
        endif()
        if(WIN32)
            target_link_libraries(tcpquic_crashpad_client INTERFACE user32 version winhttp)
        elseif(APPLE)
            target_link_libraries(tcpquic_crashpad_client INTERFACE
                "-framework ApplicationServices"
                "-framework CoreFoundation"
                "-framework CoreGraphics"
                "-framework CoreText"
                "-framework Foundation"
                "-framework IOKit"
                bsm)
        else()
            target_link_libraries(tcpquic_crashpad_client INTERFACE dl)
        endif()
        message(STATUS "Crashpad enabled: ${TCPQUIC_CRASHPAD_BUILD_DIR}")
    elseif(TCPQUIC_ENABLE_CRASHPAD_MODE STREQUAL "ON")
        message(FATAL_ERROR
            "TCPQUIC_ENABLE_CRASHPAD=ON but Crashpad was not found. "
            "Expected source at '${TCPQUIC_CRASHPAD_SOURCE_DIR}', build output at "
            "'${TCPQUIC_CRASHPAD_BUILD_DIR}', and handler '${TCPQUIC_CRASHPAD_HANDLER}'.")
    else()
        message(STATUS "Crashpad not found; crash dump handler disabled")
    endif()
endif()

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

set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/json" "${CMAKE_BINARY_DIR}/json")

set(LIBUV_BUILD_SHARED OFF CACHE BOOL "Build libuv shared library" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "Build libuv tests" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "Build libuv benchmarks" FORCE)
add_subdirectory("${TCPQUIC_LIBUV_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/libuv" EXCLUDE_FROM_ALL)
if(NOT TARGET uv_a)
    message(FATAL_ERROR "Vendored libuv did not define the required static target uv_a")
endif()

# Align tool/test binaries with msquic convention: build/bin/Release/
set(TCPQUIC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<IF:$<CONFIG:Debug>,Debug,Release>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TCPQUIC_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${TCPQUIC_OUTPUT_DIR}")

add_subdirectory(src)
