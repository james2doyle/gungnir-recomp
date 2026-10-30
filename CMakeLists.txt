cmake_minimum_required(VERSION 3.22)
project(gungnir-recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
