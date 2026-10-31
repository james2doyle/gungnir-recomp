#!/usr/bin/env python3
"""Embed SPIR-V modules as C++ arrays.

Usage: embed_shaders.py OUT_CPP OUT_HPP NAME=FILE.spv [NAME=FILE.spv ...]
"""
import sys
from pathlib import Path


def cpp_bytes(path):
    data = Path(path).read_bytes()
    if len(data) % 4:
        raise SystemExit(f"SPIR-V size is not a multiple of four: {path}")
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    return words


def main(argv):
    if len(argv) < 4:
        raise SystemExit(__doc__)
    out_cpp = Path(argv[1])
    out_hpp = Path(argv[2])
    modules = []
    for spec in argv[3:]:
        name, path = spec.split("=", 1)
        if not name.isidentifier():
            raise SystemExit(f"invalid C++ identifier: {name}")
        modules.append((name, cpp_bytes(path)))

    cpp = ['#include "ge/vulkan_shaders.hpp"', "", "namespace gungnir::ge::shaders {"]
    hpp = ["#pragma once", "", "#include <cstddef>", "#include <cstdint>", "",
           "namespace gungnir::ge::shaders {"]
    for name, words in modules:
        hpp.append(f"extern const std::uint32_t {name}[];")
        hpp.append(f"extern const std::size_t {name}_size;")
        cpp.append(f"const std::uint32_t {name}[] = {{")
        for i in range(0, len(words), 8):
            cpp.append("    " + ", ".join(f"0x{word:08X}u" for word in words[i:i + 8]) + ",")
        cpp.append("};")
        cpp.append(f"const std::size_t {name}_size = sizeof({name});")
        cpp.append("")
    hpp.append("")
    hpp.append("} // namespace gungnir::ge::shaders")
    hpp.append("")
    cpp.append("} // namespace gungnir::ge::shaders")
    cpp.append("")

    out_cpp.parent.mkdir(parents=True, exist_ok=True)
    out_hpp.parent.mkdir(parents=True, exist_ok=True)
    out_cpp.write_text("\n".join(cpp))
    out_hpp.write_text("\n".join(hpp))


if __name__ == "__main__":
    main(sys.argv)
