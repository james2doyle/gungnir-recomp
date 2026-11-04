include_guard(GLOBAL)

# Builds the shared runtime library used by the Android shell. The GE sources
# are the same files the desktop build compiles; only the platform entry point
# and the SDL/Vulkan linkage differ.
function(gungnir_add_runtime_library target)
    cmake_parse_arguments(ARG "" "ROOT;SDL3_DIR;ADRENOTOOLS_TARGET" "" ${ARGN})
    if(NOT ARG_ROOT OR NOT ARG_SDL3_DIR)
        message(FATAL_ERROR "gungnir_add_runtime_library(${target}) needs ROOT and SDL3_DIR")
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(GUNGNIR_GLSL_COMPILER NAMES glslc REQUIRED)
    if(NOT TARGET SDL3::SDL3-static)
        message(FATAL_ERROR "SDL3::SDL3-static is missing; add_subdirectory(SDL3) first")
    endif()

    set(shader_dir "${CMAKE_BINARY_DIR}/shaders/ge")
    set(gen_dir "${CMAKE_BINARY_DIR}/generated/ge")
    set(triangle_vert "${shader_dir}/triangle.vert.spv")
    set(triangle_frag "${shader_dir}/triangle.frag.spv")
    set(mesh_vert "${shader_dir}/mesh.vert.spv")
    set(mesh_frag "${shader_dir}/mesh.frag.spv")

    add_custom_command(
        OUTPUT "${triangle_vert}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
        COMMAND "${GUNGNIR_GLSL_COMPILER}" --target-env=vulkan1.1
                "${ARG_ROOT}/assets/shaders/ge/triangle.vert" -o "${triangle_vert}"
        DEPENDS "${ARG_ROOT}/assets/shaders/ge/triangle.vert"
        VERBATIM)
    add_custom_command(
        OUTPUT "${triangle_frag}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
        COMMAND "${GUNGNIR_GLSL_COMPILER}" --target-env=vulkan1.1
                "${ARG_ROOT}/assets/shaders/ge/triangle.frag" -o "${triangle_frag}"
        DEPENDS "${ARG_ROOT}/assets/shaders/ge/triangle.frag"
        VERBATIM)
    add_custom_command(
        OUTPUT "${mesh_vert}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
        COMMAND "${GUNGNIR_GLSL_COMPILER}" --target-env=vulkan1.1
                "${ARG_ROOT}/assets/shaders/ge/mesh.vert" -o "${mesh_vert}"
        DEPENDS "${ARG_ROOT}/assets/shaders/ge/mesh.vert"
        VERBATIM)
    add_custom_command(
        OUTPUT "${mesh_frag}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
        COMMAND "${GUNGNIR_GLSL_COMPILER}" --target-env=vulkan1.1
                "${ARG_ROOT}/assets/shaders/ge/mesh.frag" -o "${mesh_frag}"
        DEPENDS "${ARG_ROOT}/assets/shaders/ge/mesh.frag"
        VERBATIM)
    add_custom_command(
        OUTPUT "${gen_dir}/vulkan_shaders.cpp" "${gen_dir}/vulkan_shaders.hpp"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
        COMMAND "${Python3_EXECUTABLE}" "${ARG_ROOT}/tools/embed_shaders.py"
                "${gen_dir}/vulkan_shaders.cpp" "${gen_dir}/vulkan_shaders.hpp"
                "triangle_vert=${triangle_vert}"
                "triangle_frag=${triangle_frag}"
                "mesh_vert=${mesh_vert}"
                "mesh_frag=${mesh_frag}"
        DEPENDS "${triangle_vert}" "${triangle_frag}" "${mesh_vert}" "${mesh_frag}"
                "${ARG_ROOT}/tools/embed_shaders.py"
        VERBATIM)

    set(GUNGNIR_GAME_CORPUS "" CACHE PATH
        "Generated AOT corpus directory (build output, never committed)")
    set(corpus_sources "")
    set(corpus_include "")
    if(GUNGNIR_GAME_CORPUS)
        if(NOT EXISTS "${GUNGNIR_GAME_CORPUS}/generated_registry.cpp" OR
           NOT EXISTS "${GUNGNIR_GAME_CORPUS}/recomp_units.hpp")
            message(FATAL_ERROR
                "game_corpus_missing: ${GUNGNIR_GAME_CORPUS} needs generated_registry.cpp and recomp_units.hpp")
        endif()
        file(GLOB GAME_CORPUS_SOURCES CONFIGURE_DEPENDS
            "${GUNGNIR_GAME_CORPUS}/generated_unit_*.cpp")
        if(NOT GAME_CORPUS_SOURCES)
            message(FATAL_ERROR
                "game_corpus_missing: ${GUNGNIR_GAME_CORPUS} has no generated_unit_*.cpp")
        endif()
        list(APPEND corpus_sources
            "${GUNGNIR_GAME_CORPUS}/generated_registry.cpp"
            ${GAME_CORPUS_SOURCES}
            "${ARG_ROOT}/runtime/session/corpus_present.cpp")
        list(APPEND corpus_include "${GUNGNIR_GAME_CORPUS}")
        list(LENGTH GAME_CORPUS_SOURCES corpus_units)
        message(STATUS "gungnir: game corpus ${GUNGNIR_GAME_CORPUS} (${corpus_units} units)")
    else()
        list(APPEND corpus_sources "${ARG_ROOT}/runtime/session/corpus_absent.cpp")
        message(STATUS "gungnir: game corpus absent; runtime reports game_corpus_missing")
    endif()

    if(NOT TARGET gungnir_atrac3p)
        add_library(gungnir_atrac3p STATIC
            "${ARG_ROOT}/third_party/atrac3p/atrac3p_api.c"
            "${ARG_ROOT}/third_party/atrac3p/libavcodec/atrac.c"
            "${ARG_ROOT}/third_party/atrac3p/libavcodec/atrac3.c"
            "${ARG_ROOT}/third_party/atrac3p/libavcodec/atrac3plus.c"
            "${ARG_ROOT}/third_party/atrac3p/libavcodec/atrac3plusdec.c"
            "${ARG_ROOT}/third_party/atrac3p/libavcodec/atrac3plusdsp.c"
            "${ARG_ROOT}/third_party/atrac3p/libavcodec/bitstream.c"
            "${ARG_ROOT}/third_party/atrac3p/libavcodec/fft_float.c"
            "${ARG_ROOT}/third_party/atrac3p/libavcodec/fft_init_table.c"
            "${ARG_ROOT}/third_party/atrac3p/libavcodec/mdct_float.c"
            "${ARG_ROOT}/third_party/atrac3p/libavcodec/sinewin.c"
            "${ARG_ROOT}/third_party/atrac3p/libavutil/float_dsp.c"
            "${ARG_ROOT}/third_party/atrac3p/libavutil/intmath.c"
            "${ARG_ROOT}/third_party/atrac3p/libavutil/log2_tab.c"
            "${ARG_ROOT}/third_party/atrac3p/libavutil/mem.c"
            "${ARG_ROOT}/third_party/atrac3p/libavutil/reverse.c")
        target_include_directories(gungnir_atrac3p PUBLIC "${ARG_ROOT}/third_party/atrac3p")
        target_compile_options(gungnir_atrac3p PRIVATE -fno-strict-aliasing)
    endif()

    add_library(${target} SHARED
        "${ARG_ROOT}/runtime/audio/audio_mixer.cpp"
        "${ARG_ROOT}/runtime/audio/audio_sink.cpp"
        "${ARG_ROOT}/runtime/audio/sas_core.cpp"
        "${ARG_ROOT}/runtime/audio/atrac_file.cpp"
        "${ARG_ROOT}/runtime/hle/cadence.cpp"
        "${ARG_ROOT}/runtime/hle/hle_common.cpp"
        "${ARG_ROOT}/runtime/hle/kernel.cpp"
        "${ARG_ROOT}/runtime/hle/hle_atrac.cpp"
        "${ARG_ROOT}/runtime/hle/hle_audio.cpp"
        "${ARG_ROOT}/runtime/hle/hle_io.cpp"
        "${ARG_ROOT}/runtime/hle/hle_media.cpp"
        "${ARG_ROOT}/runtime/hle/hle_mpeg.cpp"
        "${ARG_ROOT}/runtime/hle/hle_module.cpp"
        "${ARG_ROOT}/runtime/hle/hle_sysmem.cpp"
        "${ARG_ROOT}/runtime/hle/hle_system.cpp"
        "${ARG_ROOT}/runtime/hle/hle_threadman.cpp"
        "${ARG_ROOT}/runtime/hle/hle_utility.cpp"
        "${ARG_ROOT}/runtime/savedata/save_store.cpp"
        "${ARG_ROOT}/runtime/session/game_session.cpp"
        "${ARG_ROOT}/runtime/session/diagnostics.cpp"
        "${ARG_ROOT}/runtime/session/session_lifecycle.cpp"
        "${ARG_ROOT}/runtime/session/session_report.cpp"
        "${ARG_ROOT}/runtime/session/snapshot.cpp"
        "${ARG_ROOT}/runtime/platform/android/aaudio_out.cpp"
        "${ARG_ROOT}/runtime/platform/android/audio_smoke.cpp"
        "${ARG_ROOT}/runtime/input/gamepad_profile.cpp"
        "${ARG_ROOT}/runtime/input/gamepad_remap.cpp"
        "${ARG_ROOT}/runtime/input/gamepad_state.cpp"
        "${ARG_ROOT}/runtime/input/host_controls.cpp"
        "${ARG_ROOT}/runtime/input/input_state.cpp"
        "${ARG_ROOT}/runtime/input/keyboard_map.cpp"
        "${ARG_ROOT}/runtime/input/menu_navigation.cpp"
        "${ARG_ROOT}/runtime/input/sdl_input.cpp"
        "${ARG_ROOT}/runtime/input/touch_haptics.cpp"
        "${ARG_ROOT}/runtime/input/touch_layout_store.cpp"
        "${ARG_ROOT}/runtime/input/touch_overlay.cpp"
        "${ARG_ROOT}/runtime/ge/display_list.cpp"
        "${ARG_ROOT}/runtime/ge/frame_interpolation.cpp"
        "${ARG_ROOT}/runtime/ge/frame_queue.cpp"
        "${ARG_ROOT}/runtime/ge/present_schedule.cpp"
        "${ARG_ROOT}/runtime/ge/software.cpp"
        "${ARG_ROOT}/runtime/ge/state_machine.cpp"
        "${ARG_ROOT}/runtime/ge/texture.cpp"
        "${ARG_ROOT}/runtime/ge/vertex.cpp"
        "${ARG_ROOT}/runtime/ge/vulkan_backend.cpp"
        "${ARG_ROOT}/runtime/platform/android/game_data.cpp"
        "${ARG_ROOT}/runtime/platform/android/assets.cpp"
        "${ARG_ROOT}/runtime/platform/android/affinity.cpp"
        "${ARG_ROOT}/runtime/platform/sdl_gamepad.cpp"
        "${ARG_ROOT}/runtime/platform/adreno/loader.cpp"
        "${ARG_ROOT}/runtime/platform/adreno/package.cpp"
        "${ARG_ROOT}/third_party/miniz/miniz.c"
        "${ARG_ROOT}/third_party/yakumo/src/decoder.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/deflate.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/elf32.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/guest_memory.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/interpreter.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/iso_image.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/nid_registry.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/param_sfo.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/program_analysis.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/runtime.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/sha256.cpp"
        "${ARG_ROOT}/third_party/yakumo/src/utf8_path.cpp"
        "${ARG_ROOT}/third_party/volk/volk.c"
        ${corpus_sources}
        "${gen_dir}/vulkan_shaders.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/main.cpp")
    target_include_directories(${target} PRIVATE
        "${ARG_ROOT}/runtime"
        "${CMAKE_BINARY_DIR}/generated"
        "${ARG_ROOT}/third_party/miniz"
        "${ARG_ROOT}/third_party/nlohmann-json"
        "${ARG_ROOT}/third_party/yakumo/include"
        "${ARG_ROOT}/third_party/yakumo/src"
        "${ARG_ROOT}/third_party/atrac3p"
        ${corpus_include}
        "${ARG_ROOT}/third_party/volk")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall -Wextra -O2 -fno-lto)
    target_link_libraries(${target} PRIVATE SDL3::SDL3-static vulkan android aaudio log z)
    target_link_libraries(${target} PRIVATE gungnir_atrac3p)
    target_compile_definitions(${target} PRIVATE GUNGNIR_HAVE_SDL3=1)
    if(ARG_ADRENOTOOLS_TARGET)
        target_link_libraries(${target} PRIVATE ${ARG_ADRENOTOOLS_TARGET})
        target_compile_definitions(${target} PRIVATE GUNGNIR_HAVE_ADRENOTOOLS=1)
    else()
        target_compile_definitions(${target} PRIVATE GUNGNIR_HAVE_ADRENOTOOLS=0)
    endif()

    set(GUNGNIR_ANDROID_O1_SOURCES "" CACHE STRING
        "Semicolon-separated runtime sources compiled at -O1")
    foreach(source IN LISTS GUNGNIR_ANDROID_O1_SOURCES)
        if(NOT IS_ABSOLUTE "${source}")
            set(source "${ARG_ROOT}/${source}")
        endif()
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "GUNGNIR_ANDROID_O1_SOURCES entry does not exist: ${source}")
        endif()
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-O1")
        message(STATUS "gungnir: -O1 override for ${source}")
    endforeach()
endfunction()
