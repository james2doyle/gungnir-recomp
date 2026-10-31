include_guard(GLOBAL)

# Builds the shared runtime library used by the Android shell. The GE sources
# are the same files the desktop build compiles; only the platform entry point
# and the SDL/Vulkan linkage differ.
function(gungnir_add_runtime_library target)
    cmake_parse_arguments(ARG "" "ROOT;SDL3_DIR" "" ${ARGN})
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
        "${ARG_ROOT}/runtime/ge/display_list.cpp"
        "${ARG_ROOT}/runtime/ge/software.cpp"
        "${ARG_ROOT}/runtime/ge/texture.cpp"
        "${ARG_ROOT}/runtime/ge/vertex.cpp"
        "${ARG_ROOT}/runtime/ge/vulkan_backend.cpp"
        "${ARG_ROOT}/runtime/platform/android/game_data.cpp"
        "${ARG_ROOT}/runtime/platform/android/assets.cpp"
        "${gen_dir}/vulkan_shaders.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/main.cpp")
    target_include_directories(${target} PRIVATE "${ARG_ROOT}/runtime" "${CMAKE_BINARY_DIR}/generated")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    target_link_libraries(${target} PRIVATE SDL3::SDL3-static vulkan android log z)
endfunction()
