function(tcpquic_bootstrap_crashpad source_dir build_dir out_source_dir out_build_dir)
    if(NOT EXISTS "${source_dir}/DEPS")
        message(FATAL_ERROR "Crashpad source tree is missing DEPS: ${source_dir}")
    endif()

    find_package(Git REQUIRED)

    set(depot_tools_dir "${CMAKE_BINARY_DIR}/_deps/depot_tools")
    find_program(TCPQUIC_GCLIENT_EXECUTABLE gclient)
    if(NOT TCPQUIC_GCLIENT_EXECUTABLE)
        if(NOT EXISTS "${depot_tools_dir}/gclient")
            message(STATUS "Fetching depot_tools for Crashpad bootstrap: ${depot_tools_dir}")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "${depot_tools_dir}"
                RESULT_VARIABLE depot_tools_result)
            if(NOT depot_tools_result EQUAL 0)
                message(FATAL_ERROR "Failed to fetch depot_tools for Crashpad bootstrap")
            endif()
        endif()
        set(TCPQUIC_GCLIENT_EXECUTABLE "${depot_tools_dir}/gclient")
    endif()

    get_filename_component(gclient_dir "${TCPQUIC_GCLIENT_EXECUTABLE}" DIRECTORY)
    set(crashpad_tool_path "${gclient_dir}:$ENV{PATH}")

    if(NOT EXISTS "${source_dir}/.gclient")
        message(STATUS "Configuring Crashpad gclient workspace in ${source_dir}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "PATH=${crashpad_tool_path}"
                "${TCPQUIC_GCLIENT_EXECUTABLE}" config --unmanaged https://chromium.googlesource.com/crashpad/crashpad
            WORKING_DIRECTORY "${source_dir}"
            RESULT_VARIABLE gclient_config_result)
        if(NOT gclient_config_result EQUAL 0)
            message(FATAL_ERROR "Failed to configure Crashpad gclient workspace")
        endif()
    endif()

    set(bootstrap_source_dir "${source_dir}")
    if(EXISTS "${source_dir}/crashpad/client/crashpad_client.h")
        set(bootstrap_source_dir "${source_dir}/crashpad")
    endif()

    if(NOT EXISTS "${bootstrap_source_dir}/third_party/mini_chromium/mini_chromium/base/files/file_path.h")
        message(STATUS "Syncing Crashpad DEPS in ${source_dir}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "PATH=${crashpad_tool_path}"
                "${TCPQUIC_GCLIENT_EXECUTABLE}" sync --nohooks
            WORKING_DIRECTORY "${source_dir}"
            RESULT_VARIABLE gclient_sync_result)
        if(NOT gclient_sync_result EQUAL 0)
            message(FATAL_ERROR "Failed to sync Crashpad DEPS")
        endif()
    endif()

    if(EXISTS "${source_dir}/crashpad/client/crashpad_client.h")
        set(bootstrap_source_dir "${source_dir}/crashpad")
    endif()

    if(build_dir STREQUAL "${source_dir}/out/Default" AND NOT bootstrap_source_dir STREQUAL "${source_dir}")
        set(bootstrap_build_dir "${bootstrap_source_dir}/out/Default")
    else()
        set(bootstrap_build_dir "${build_dir}")
    endif()

    find_program(TCPQUIC_GN_EXECUTABLE gn
        PATHS
            "${source_dir}/buildtools/mac"
            "${source_dir}/buildtools/linux64"
            "${source_dir}/buildtools/win"
            "${depot_tools_dir}"
        NO_DEFAULT_PATH)
    if(NOT TCPQUIC_GN_EXECUTABLE)
        find_program(TCPQUIC_GN_EXECUTABLE gn)
    endif()
    if(NOT TCPQUIC_GN_EXECUTABLE)
        message(FATAL_ERROR "Crashpad bootstrap requires gn from depot_tools")
    endif()

    find_program(TCPQUIC_NINJA_EXECUTABLE ninja)
    if(NOT TCPQUIC_NINJA_EXECUTABLE AND EXISTS "${bootstrap_source_dir}/third_party/ninja/ninja")
        set(TCPQUIC_NINJA_EXECUTABLE "${bootstrap_source_dir}/third_party/ninja/ninja")
    endif()
    if(NOT TCPQUIC_NINJA_EXECUTABLE)
        message(FATAL_ERROR "Crashpad bootstrap requires ninja")
    endif()

    if(NOT EXISTS "${bootstrap_build_dir}/crashpad_handler")
        message(STATUS "Generating Crashpad GN build: ${bootstrap_build_dir}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "PATH=${crashpad_tool_path}"
                "${TCPQUIC_GN_EXECUTABLE}" gen "${bootstrap_build_dir}" --args=is_debug=false
            WORKING_DIRECTORY "${bootstrap_source_dir}"
            RESULT_VARIABLE gn_gen_result)
        if(NOT gn_gen_result EQUAL 0)
            message(FATAL_ERROR "Failed to generate Crashpad GN build")
        endif()

        message(STATUS "Building Crashpad runtime libraries and tools")
        execute_process(
            COMMAND "${TCPQUIC_NINJA_EXECUTABLE}" -C "${bootstrap_build_dir}"
                crashpad_handler crashpad_database_util dump_minidump_annotations
            WORKING_DIRECTORY "${bootstrap_source_dir}"
            RESULT_VARIABLE ninja_result)
        if(NOT ninja_result EQUAL 0)
            message(FATAL_ERROR "Failed to build Crashpad")
        endif()
    endif()

    set(${out_source_dir} "${bootstrap_source_dir}" PARENT_SCOPE)
    set(${out_build_dir} "${bootstrap_build_dir}" PARENT_SCOPE)
endfunction()
