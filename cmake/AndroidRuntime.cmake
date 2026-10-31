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

    add_library(${target} SHARED
        "${ARG_ROOT}/runtime/input/input_state.cpp"
        "${ARG_ROOT}/runtime/input/keyboard_map.cpp"
        "${ARG_ROOT}/runtime/input/sdl_input.cpp"
        "${ARG_ROOT}/runtime/input/touch_haptics.cpp"
        "${ARG_ROOT}/runtime/input/touch_layout_store.cpp"
        "${ARG_ROOT}/runtime/input/touch_overlay.cpp"
        "${ARG_ROOT}/runtime/ge/display_list.cpp"
        "${ARG_ROOT}/runtime/ge/software.cpp"
        "${ARG_ROOT}/runtime/ge/texture.cpp"
        "${ARG_ROOT}/runtime/ge/vertex.cpp"
        "${ARG_ROOT}/runtime/ge/vulkan_backend.cpp"
        "${ARG_ROOT}/runtime/platform/android/game_data.cpp"
        "${ARG_ROOT}/runtime/platform/android/assets.cpp"
        "${ARG_ROOT}/runtime/platform/android/affinity.cpp"
        "${ARG_ROOT}/runtime/platform/adreno/loader.cpp"
        "${ARG_ROOT}/runtime/platform/adreno/package.cpp"
        "${ARG_ROOT}/third_party/miniz/miniz.c"
        "${ARG_ROOT}/third_party/yakumo/src/sha256.cpp"
        "${ARG_ROOT}/third_party/volk/volk.c"
        "${gen_dir}/vulkan_shaders.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/main.cpp")
    target_include_directories(${target} PRIVATE
        "${ARG_ROOT}/runtime"
        "${CMAKE_BINARY_DIR}/generated"
        "${ARG_ROOT}/third_party/miniz"
        "${ARG_ROOT}/third_party/nlohmann-json"
        "${ARG_ROOT}/third_party/yakumo/include"
        "${ARG_ROOT}/third_party/volk")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall -Wextra -O2 -fno-lto)
    target_link_libraries(${target} PRIVATE SDL3::SDL3-static vulkan android log z)
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
