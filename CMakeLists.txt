cmake_minimum_required(VERSION 3.22)
project(gungnir-recomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- Vendored third_party (see third_party/yakumo/PROVENANCE.md) ----------
# PSPRecomp framework core subset + readers, all MIT, byte-identical.
add_library(yakumo STATIC
    third_party/yakumo/src/decoder.cpp
    third_party/yakumo/src/deflate.cpp
    third_party/yakumo/src/elf32.cpp
    third_party/yakumo/src/guest_memory.cpp
    third_party/yakumo/src/interpreter.cpp
    third_party/yakumo/src/iso_image.cpp
    third_party/yakumo/src/nid_registry.cpp
    third_party/yakumo/src/param_sfo.cpp
    third_party/yakumo/src/program_analysis.cpp
    third_party/yakumo/src/runtime.cpp
    third_party/yakumo/src/sha256.cpp
    third_party/yakumo/src/utf8_path.cpp)
target_include_directories(yakumo PUBLIC
    third_party/yakumo/include
    third_party/yakumo/src)

# --- Input core (T11) ------------------------------------------------------
# Source-neutral button/analog state and the PSP pad mapping. No SDL, JNI or
# guest-memory dependency, so the merge and edge contracts run on the host.
add_library(gungnir_input STATIC
    runtime/input/gamepad_profile.cpp
    runtime/input/gamepad_remap.cpp
    runtime/input/gamepad_state.cpp
    runtime/input/host_controls.cpp
    runtime/input/input_state.cpp
    runtime/input/keyboard_map.cpp
    runtime/input/menu_navigation.cpp
    runtime/input/touch_haptics.cpp
    runtime/input/touch_layout_store.cpp
    runtime/input/touch_overlay.cpp)
target_include_directories(gungnir_input PUBLIC runtime)
target_include_directories(gungnir_input PRIVATE ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json)

add_executable(input_tests tests/input/input_tests.cpp)
target_link_libraries(input_tests PRIVATE gungnir_input)
set_target_properties(input_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/input)

add_executable(host_controls_tests tests/input/host_controls_tests.cpp)
target_link_libraries(host_controls_tests PRIVATE gungnir_input)
set_target_properties(host_controls_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/input)

add_executable(touch_overlay_tests tests/input/touch_overlay_tests.cpp)
target_link_libraries(touch_overlay_tests PRIVATE gungnir_input)
target_include_directories(touch_overlay_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json)
set_target_properties(touch_overlay_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/input)

add_executable(touch_haptics_tests tests/input/touch_haptics_tests.cpp)
target_link_libraries(touch_haptics_tests PRIVATE gungnir_input)
set_target_properties(touch_haptics_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/input)

add_executable(gamepad_tests tests/input/gamepad_tests.cpp)
target_link_libraries(gamepad_tests PRIVATE gungnir_input)
set_target_properties(gamepad_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/input)

add_executable(menu_navigation_tests tests/input/menu_navigation_tests.cpp)
target_link_libraries(menu_navigation_tests PRIVATE gungnir_input)
set_target_properties(menu_navigation_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/input)

# --- HLE runtime (T6) -------------------------------------------------------
add_library(gungnir_hle STATIC
    runtime/audio/atrac_file.cpp
    runtime/audio/audio_mixer.cpp
    runtime/audio/audio_sink.cpp
    runtime/audio/sas_core.cpp
    runtime/hle/cadence.cpp
    runtime/hle/hle_common.cpp
    runtime/hle/kernel.cpp
    runtime/hle/hle_atrac.cpp
    runtime/hle/hle_audio.cpp
    runtime/hle/hle_sysmem.cpp
    runtime/hle/hle_threadman.cpp
    runtime/hle/hle_system.cpp
    runtime/hle/hle_io.cpp
    runtime/hle/hle_mpeg.cpp
    runtime/hle/hle_utility.cpp
    runtime/hle/hle_media.cpp
    runtime/hle/hle_module.cpp
    runtime/savedata/save_store.cpp
    runtime/session/game_session.cpp
    runtime/session/diagnostics.cpp
    runtime/session/session_lifecycle.cpp
    runtime/session/session_report.cpp
    runtime/session/snapshot.cpp
    runtime/session/corpus_present.cpp)
target_include_directories(gungnir_hle PUBLIC runtime)
target_link_libraries(gungnir_hle PUBLIC yakumo gungnir_ge gungnir_input atrac3p)

add_executable(hle_module_tests tests/hle/module_tests.cpp)
target_link_libraries(hle_module_tests PRIVATE gungnir_hle)
set_target_properties(hle_module_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/hle)

add_executable(hle_media_tests tests/hle/media_tests.cpp)
target_link_libraries(hle_media_tests PRIVATE gungnir_hle)
set_target_properties(hle_media_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/hle)

add_executable(hle_kernel_tests tests/hle/kernel_tests.cpp)
target_link_libraries(hle_kernel_tests PRIVATE gungnir_hle)
set_target_properties(hle_kernel_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/hle)

add_executable(hle_import_tests tests/hle/hle_import_tests.cpp)
target_link_libraries(hle_import_tests PRIVATE gungnir_hle)
set_target_properties(hle_import_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/hle)

add_executable(savedata_tests tests/savedata/savedata_tests.cpp)
target_link_libraries(savedata_tests PRIVATE gungnir_hle)
set_target_properties(savedata_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/savedata)

add_executable(session_report_tests tests/session/session_report_tests.cpp)
target_link_libraries(session_report_tests PRIVATE gungnir_hle)
set_target_properties(session_report_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/session)

add_executable(session_lifecycle_tests tests/session/session_lifecycle_tests.cpp)
target_link_libraries(session_lifecycle_tests PRIVATE gungnir_hle)
set_target_properties(session_lifecycle_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/session)

add_executable(snapshot_tests tests/session/snapshot_tests.cpp)
target_link_libraries(snapshot_tests PRIVATE gungnir_hle)
set_target_properties(snapshot_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/session)

add_executable(snapshot_kernel_tests tests/session/snapshot_kernel_tests.cpp)
target_link_libraries(snapshot_kernel_tests PRIVATE gungnir_hle)
set_target_properties(snapshot_kernel_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/session)

add_executable(snapshot_hle_tests tests/session/snapshot_hle_tests.cpp)
target_link_libraries(snapshot_hle_tests PRIVATE gungnir_hle)
target_compile_definitions(snapshot_hle_tests PRIVATE
    GUNGNIR_SNAPSHOT_NIDS_CSV="${CMAKE_SOURCE_DIR}/configs/nids.csv")
set_target_properties(snapshot_hle_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/session)

# --- GE command stream (T7a) ----------------------------------------------
add_library(gungnir_ge STATIC
    runtime/ge/display_list.cpp
    runtime/ge/frame_queue.cpp
    runtime/ge/software.cpp
    runtime/ge/state_machine.cpp
    runtime/ge/targets.cpp
    runtime/ge/texture.cpp
    runtime/ge/vertex.cpp)
target_include_directories(gungnir_ge PUBLIC runtime)

add_executable(ge_display_list_tests tests/ge/display_list_tests.cpp)
target_link_libraries(ge_display_list_tests PRIVATE gungnir_ge)
set_target_properties(ge_display_list_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/ge)

add_executable(ge_vertex_tests tests/ge/vertex_tests.cpp)
target_link_libraries(ge_vertex_tests PRIVATE gungnir_ge yakumo)
set_target_properties(ge_vertex_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/ge)

add_executable(ge_texture_tests tests/ge/texture_tests.cpp)
target_link_libraries(ge_texture_tests PRIVATE gungnir_ge)
set_target_properties(ge_texture_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/ge)

add_executable(ge_software_tests tests/ge/software_tests.cpp)
target_link_libraries(ge_software_tests PRIVATE gungnir_ge)
set_target_properties(ge_software_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/ge)

add_executable(ge_state_machine_tests tests/ge/state_machine_tests.cpp)
target_link_libraries(ge_state_machine_tests PRIVATE gungnir_ge)
set_target_properties(ge_state_machine_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/ge)

add_executable(ge_targets_tests tests/ge/targets_tests.cpp)
target_link_libraries(ge_targets_tests PRIVATE gungnir_ge)
set_target_properties(ge_targets_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/ge)

# --- Texture containers (T8a) ---------------------------------------------
add_library(gungnir_tex STATIC runtime/tex/tpl.cpp)
target_include_directories(gungnir_tex PUBLIC runtime)
target_link_libraries(gungnir_tex PUBLIC gungnir_ge)

add_executable(tex_tpl_tests tests/tex/tpl_tests.cpp)
target_link_libraries(tex_tpl_tests PRIVATE gungnir_tex)
set_target_properties(tex_tpl_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/tex)

find_package(PNG QUIET)
if(PNG_FOUND)
    add_library(gungnir_tex_png STATIC runtime/tex/png.cpp)
    target_include_directories(gungnir_tex_png PUBLIC runtime)
    target_link_libraries(gungnir_tex_png PUBLIC PNG::PNG)

    add_library(gungnir_tex_dump STATIC runtime/tex/dump.cpp)
    target_include_directories(gungnir_tex_dump PUBLIC runtime)
    target_link_libraries(gungnir_tex_dump PUBLIC gungnir_tex gungnir_tex_png)

    add_executable(tex_dump_tests tests/tex/dump_tests.cpp)
    target_link_libraries(tex_dump_tests PRIVATE gungnir_tex_dump)
    set_target_properties(tex_dump_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/tex)

    add_library(gungnir_tex_replace STATIC runtime/tex/replace.cpp)
    target_include_directories(gungnir_tex_replace PUBLIC runtime)
    target_link_libraries(gungnir_tex_replace PUBLIC gungnir_tex gungnir_tex_png)

    add_executable(tex_replace_tests tests/tex/replace_tests.cpp)
    target_link_libraries(tex_replace_tests PRIVATE gungnir_tex_replace gungnir_tex_dump)
    set_target_properties(tex_replace_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/tex)
endif()

# --- Android game-data intake (T9c) ---------------------------------------
find_package(ZLIB REQUIRED)
add_library(gungnir_platform STATIC
    runtime/platform/android/game_data.cpp
    runtime/platform/android/assets.cpp
    runtime/platform/android/affinity.cpp)
target_include_directories(gungnir_platform PUBLIC runtime)
target_link_libraries(gungnir_platform PUBLIC ZLIB::ZLIB)

add_executable(game_data_tests tests/platform/game_data_tests.cpp)
target_link_libraries(game_data_tests PRIVATE gungnir_platform)
set_target_properties(game_data_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/platform)

add_executable(affinity_tests tests/platform/affinity_tests.cpp)
target_link_libraries(affinity_tests PRIVATE gungnir_platform)
set_target_properties(affinity_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/platform)

# --- Adreno driver packages (T10b) ----------------------------------------
add_library(gungnir_adreno STATIC
    runtime/platform/adreno/package.cpp
    runtime/platform/adreno/loader.cpp
    third_party/miniz/miniz.c)
target_include_directories(gungnir_adreno PUBLIC
    runtime
    third_party/miniz
    third_party/nlohmann-json)
target_link_libraries(gungnir_adreno PUBLIC yakumo)

add_executable(driver_package_tests tests/platform/driver_package_tests.cpp)
target_link_libraries(driver_package_tests PRIVATE gungnir_adreno)
set_target_properties(driver_package_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/platform)

add_executable(loader_tests tests/platform/loader_tests.cpp)
target_link_libraries(loader_tests PRIVATE gungnir_adreno)
set_target_properties(loader_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/platform)

# --- Post-processing chain (T7g) ------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(JSONC QUIET IMPORTED_TARGET json-c)
endif()
if(JSONC_FOUND)
    add_library(gungnir_post STATIC runtime/post/post.cpp)
    target_include_directories(gungnir_post PUBLIC runtime)
    target_link_libraries(gungnir_post PUBLIC PkgConfig::JSONC)

    add_executable(post_tests tests/post/post_tests.cpp)
    target_link_libraries(post_tests PRIVATE gungnir_post)
    set_target_properties(post_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/post)

    find_program(POST_GLSL_COMPILER NAMES glslc)
    if(POST_GLSL_COMPILER)
        set(POST_SHADER_DIR ${CMAKE_BINARY_DIR}/shaders/post)
        set(POST_SHADER_SOURCES
            fullscreen.vert integer_scale.frag fsr_lite.frag scanlines.frag lcd_grid.frag)
        set(POST_SHADER_SPV)
        foreach(shader ${POST_SHADER_SOURCES})
            get_filename_component(shader_name ${shader} NAME)
            set(output ${POST_SHADER_DIR}/${shader_name}.spv)
            add_custom_command(
                OUTPUT ${output}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${POST_SHADER_DIR}
                COMMAND ${POST_GLSL_COMPILER} --target-env=vulkan1.1
                        ${CMAKE_SOURCE_DIR}/assets/shaders/post/${shader} -o ${output}
                DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/post/${shader}
                VERBATIM)
            list(APPEND POST_SHADER_SPV ${output})
        endforeach()
        add_custom_target(post_shaders DEPENDS ${POST_SHADER_SPV})
        add_dependencies(post_tests post_shaders)
    endif()
endif()

# --- Vulkan GE backend (T7e) ----------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
find_package(Vulkan QUIET)
find_program(GLSL_COMPILER NAMES glslc)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3)
endif()

if(SDL3_FOUND)
    add_executable(sdl_gamepad_tests
        tests/input/sdl_gamepad_tests.cpp
        runtime/platform/sdl_gamepad.cpp)
    target_compile_definitions(sdl_gamepad_tests PRIVATE GUNGNIR_HAVE_SDL3=1)
    target_include_directories(sdl_gamepad_tests PRIVATE runtime)
    target_link_libraries(sdl_gamepad_tests PRIVATE gungnir_input PkgConfig::SDL3)
    set_target_properties(sdl_gamepad_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/input)

    add_executable(sdl_audio_smoke
        tests/audio/sdl_audio_smoke.cpp
        runtime/platform/sdl_audio.cpp)
    target_compile_definitions(sdl_audio_smoke PRIVATE GUNGNIR_HAVE_SDL3=1)
    target_include_directories(sdl_audio_smoke PRIVATE runtime)
    target_link_libraries(sdl_audio_smoke PRIVATE gungnir_hle PkgConfig::SDL3)
    set_target_properties(sdl_audio_smoke PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/audio)
endif()

if(Vulkan_FOUND AND GLSL_COMPILER)
    set(GE_SHADER_DIR ${CMAKE_BINARY_DIR}/shaders/ge)
    set(GE_SHADER_GEN_DIR ${CMAKE_BINARY_DIR}/generated/ge)
    set(GE_TRIANGLE_VERT_SPV ${GE_SHADER_DIR}/triangle.vert.spv)
    set(GE_TRIANGLE_FRAG_SPV ${GE_SHADER_DIR}/triangle.frag.spv)
    set(GE_MESH_VERT_SPV ${GE_SHADER_DIR}/mesh.vert.spv)
    set(GE_MESH_FRAG_SPV ${GE_SHADER_DIR}/mesh.frag.spv)
    add_custom_command(
        OUTPUT ${GE_TRIANGLE_VERT_SPV}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GE_SHADER_DIR}
        COMMAND ${GLSL_COMPILER} --target-env=vulkan1.1
                ${CMAKE_SOURCE_DIR}/assets/shaders/ge/triangle.vert -o ${GE_TRIANGLE_VERT_SPV}
        DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ge/triangle.vert
        VERBATIM)
    add_custom_command(
        OUTPUT ${GE_TRIANGLE_FRAG_SPV}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GE_SHADER_DIR}
        COMMAND ${GLSL_COMPILER} --target-env=vulkan1.1
                ${CMAKE_SOURCE_DIR}/assets/shaders/ge/triangle.frag -o ${GE_TRIANGLE_FRAG_SPV}
        DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ge/triangle.frag
        VERBATIM)
    add_custom_command(
        OUTPUT ${GE_MESH_VERT_SPV}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GE_SHADER_DIR}
        COMMAND ${GLSL_COMPILER} --target-env=vulkan1.1
                ${CMAKE_SOURCE_DIR}/assets/shaders/ge/mesh.vert -o ${GE_MESH_VERT_SPV}
        DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ge/mesh.vert
        VERBATIM)
    add_custom_command(
        OUTPUT ${GE_MESH_FRAG_SPV}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GE_SHADER_DIR}
        COMMAND ${GLSL_COMPILER} --target-env=vulkan1.1
                ${CMAKE_SOURCE_DIR}/assets/shaders/ge/mesh.frag -o ${GE_MESH_FRAG_SPV}
        DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ge/mesh.frag
        VERBATIM)
    add_custom_command(
        OUTPUT ${GE_SHADER_GEN_DIR}/vulkan_shaders.cpp ${GE_SHADER_GEN_DIR}/vulkan_shaders.hpp
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed_shaders.py
                ${GE_SHADER_GEN_DIR}/vulkan_shaders.cpp ${GE_SHADER_GEN_DIR}/vulkan_shaders.hpp
                triangle_vert=${GE_TRIANGLE_VERT_SPV}
                triangle_frag=${GE_TRIANGLE_FRAG_SPV}
                mesh_vert=${GE_MESH_VERT_SPV}
                mesh_frag=${GE_MESH_FRAG_SPV}
        DEPENDS ${GE_TRIANGLE_VERT_SPV} ${GE_TRIANGLE_FRAG_SPV} ${GE_MESH_VERT_SPV} ${GE_MESH_FRAG_SPV}
                ${CMAKE_SOURCE_DIR}/tools/embed_shaders.py
        VERBATIM)

    add_library(gungnir_ge_vulkan STATIC
        runtime/ge/vulkan_backend.cpp
        ${GE_SHADER_GEN_DIR}/vulkan_shaders.cpp)
    target_include_directories(gungnir_ge_vulkan PUBLIC runtime ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(gungnir_ge_vulkan PUBLIC gungnir_ge Vulkan::Vulkan)

    add_executable(ge_vulkan_tests tests/ge/vulkan_tests.cpp)
    target_link_libraries(ge_vulkan_tests PRIVATE gungnir_ge_vulkan)
    if(SDL3_FOUND)
        target_link_libraries(ge_vulkan_tests PRIVATE PkgConfig::SDL3)
        target_compile_definitions(gungnir_ge_vulkan PUBLIC GUNGNIR_HAVE_SDL3=1)
        target_compile_definitions(ge_vulkan_tests PRIVATE GUNGNIR_HAVE_SDL3=1)
    endif()
    set_target_properties(ge_vulkan_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/ge)
endif()

# --- ATRAC3/3+ decoder (FFmpeg n4.4 subset, vendored via Nakagawa) ----------
add_library(atrac3p STATIC
    third_party/atrac3p/atrac3p_api.c
    third_party/atrac3p/libavcodec/atrac.c
    third_party/atrac3p/libavcodec/atrac3.c
    third_party/atrac3p/libavcodec/atrac3plus.c
    third_party/atrac3p/libavcodec/atrac3plusdec.c
    third_party/atrac3p/libavcodec/atrac3plusdsp.c
    third_party/atrac3p/libavcodec/bitstream.c
    third_party/atrac3p/libavcodec/fft_float.c
    third_party/atrac3p/libavcodec/fft_init_table.c
    third_party/atrac3p/libavcodec/mdct_float.c
    third_party/atrac3p/libavcodec/sinewin.c
    third_party/atrac3p/libavutil/float_dsp.c
    third_party/atrac3p/libavutil/intmath.c
    third_party/atrac3p/libavutil/log2_tab.c
    third_party/atrac3p/libavutil/mem.c
    third_party/atrac3p/libavutil/reverse.c)
target_include_directories(atrac3p PUBLIC third_party/atrac3p)
target_compile_options(atrac3p PRIVATE -fno-strict-aliasing)

add_executable(atrac3p_selftest tests/audio/atrac3p_selftest.cpp)
target_link_libraries(atrac3p_selftest PRIVATE atrac3p)
set_target_properties(atrac3p_selftest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/audio)

add_executable(audio_mixer_tests tests/audio/mixer_tests.cpp)
target_link_libraries(audio_mixer_tests PRIVATE gungnir_hle)
set_target_properties(audio_mixer_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/audio)

add_executable(atrac_track_tests tests/audio/atrac_track_tests.cpp)
target_link_libraries(atrac_track_tests PRIVATE gungnir_hle)
set_target_properties(atrac_track_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/audio)

# --- Analysis / codegen tooling (T4, T5) -----------------------------------
add_executable(yakumo_analyzer third_party/yakumo/tools/analyzer_main.cpp)
target_link_libraries(yakumo_analyzer PRIVATE yakumo)

add_executable(yakumo_codegen third_party/yakumo/tools/codegen_main.cpp)
target_link_libraries(yakumo_codegen PRIVATE yakumo)

add_executable(yakumo_dump_function third_party/yakumo/tools/dump_function.cpp)
target_link_libraries(yakumo_dump_function PRIVATE yakumo)
set_target_properties(yakumo_analyzer yakumo_codegen yakumo_dump_function PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

# --- Intake reader tool (T3) ---------------------------------------------
# Subcommands: iso-extract, elf-inspect, sfo-info. Line-oriented protocol on
# stdout; named errors ("ERR <NAME> ...") on stderr. The orchestrator is
# tools/intake/intake.py.
add_executable(intake_tool tools/intake/intake_tool.cpp)
target_link_libraries(intake_tool PRIVATE yakumo)
set_target_properties(intake_tool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools/intake)

# --- Core fixture harness (T4) ---------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
file(GLOB CORE_FIXTURE_SOURCES CONFIGURE_DEPENDS tests/core/fixtures/*.mips)
foreach(fixture_src ${CORE_FIXTURE_SOURCES})
    get_filename_component(fixture_name ${fixture_src} NAME_WE)
    set(fixture_dir ${CMAKE_BINARY_DIR}/tests/core/${fixture_name})
    set(fixture_elf ${fixture_dir}/${fixture_name}.elf)
    set(fixture_csv ${fixture_dir}/${fixture_name}.csv)
    set(fixture_json ${fixture_dir}/${fixture_name}.json)
    set(fixture_gen ${fixture_dir}/${fixture_name}_gen.cpp)
    add_custom_command(
        OUTPUT ${fixture_elf} ${fixture_csv} ${fixture_json}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${fixture_dir}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/core/assemble.py
                ${fixture_src} ${fixture_dir}/${fixture_name}
        DEPENDS ${fixture_src} ${CMAKE_SOURCE_DIR}/tests/core/assemble.py
        VERBATIM)
    add_custom_command(
        OUTPUT ${fixture_gen}
        COMMAND $<TARGET_FILE:yakumo_codegen> ${fixture_elf} ${fixture_csv} ${fixture_gen}
        DEPENDS yakumo_codegen ${fixture_elf} ${fixture_csv}
        VERBATIM)
    add_executable(core_harness_${fixture_name} tests/core/harness.cpp ${fixture_gen})
    target_link_libraries(core_harness_${fixture_name} PRIVATE yakumo)
    set_target_properties(core_harness_${fixture_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/core)
endforeach()

# --- Test wiring -----------------------------------------------------------
enable_testing()
add_test(NAME python_suites
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -p "test_*.py"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# T13d: private-input probe. Exit 77 (input absent) maps to
# Skipped; never a pass. Strict-mode fail-closed lives in
# tests/private_inputs.py for real tests.
add_test(NAME private_inputs
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/private_input_probe.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(private_inputs PROPERTIES SKIP_RETURN_CODE 77)
