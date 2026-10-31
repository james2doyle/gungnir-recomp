cmake_minimum_required(VERSION 3.22.1)
project(gungnir_probe LANGUAGES CXX)

add_library(gungnir_probe SHARED src/main/cpp/probe.cpp)
target_compile_features(gungnir_probe PRIVATE cxx_std_20)
target_compile_options(gungnir_probe PRIVATE -Wall -Wextra)
