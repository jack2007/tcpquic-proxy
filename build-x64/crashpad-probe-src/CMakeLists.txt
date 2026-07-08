cmake_minimum_required(VERSION 3.20)
project(tcpquic_crashpad_probe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(tcpquic_crashpad_probe
    C:/src/tcpquic-proxy/build-x64/crashpad-probe-src/probe.cpp
    C:/src/tcpquic-proxy/src/platform/crash_dump.cpp
    C:/src/tcpquic-proxy/src/platform/exe_path.cpp
)

target_compile_definitions(tcpquic_crashpad_probe PRIVATE
    TCPQUIC_HAS_CRASHPAD=1
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_include_directories(tcpquic_crashpad_probe PRIVATE
    C:/src/tcpquic-proxy/src/platform
    C:/src/tcpquic-proxy/third_party/crashpad
    C:/src/tcpquic-proxy/third_party/crashpad/third_party/mini_chromium/mini_chromium
)

target_link_libraries(tcpquic_crashpad_probe PRIVATE
    C:/src/tcpquic-proxy/third_party/crashpad/out/MD/obj/client/client.lib
    C:/src/tcpquic-proxy/third_party/crashpad/out/MD/obj/util/util.lib
    C:/src/tcpquic-proxy/third_party/crashpad/out/MD/obj/third_party/mini_chromium/mini_chromium/base/base.lib
    rpcrt4
    user32
    version
    winhttp
)

if(MSVC)
    set_property(TARGET tcpquic_crashpad_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

add_custom_command(TARGET tcpquic_crashpad_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        C:/src/tcpquic-proxy/third_party/crashpad/out/MD/crashpad_handler.exe
        $<TARGET_FILE_DIR:tcpquic_crashpad_probe>
)
