# SHADERS — GE, Vulkan and post-processing

All shader sources are original and live under `assets/shaders/`. SPIR-V is
build output: never committed, never hand-edited.

## Build

- `glslc --target-env=vulkan1.1` compiles each GLSL file to an `.spv` under
  `build/shaders/`.
- `tools/embed_shaders.py` turns the SPIR-V words into C++ arrays in
  `build/generated/ge/vulkan_shaders.{hpp,cpp}`.
- `gungnir_ge_vulkan` consumes those arrays; a host without `glslc` skips the
  Vulkan target rather than silently using stale blobs.
- Post shaders are compiled by the `post_shaders` target as a syntax check;
  the CPU post chain remains the current parity path.

Manual compile check:

```sh
glslc --target-env=vulkan1.1 assets/shaders/ge/mesh.vert -o /tmp/mesh.vert.spv
glslc --target-env=vulkan1.1 assets/shaders/post/fsr_lite.frag -o /tmp/fsr.frag.spv
```

## GE shaders

### `ge/triangle.vert` + `ge/triangle.frag`

First-light pipeline for T7e. A full-screen vertex buffer with colour
attributes; used only by the headless/swapchain smoke test.

### `ge/mesh.vert` + `ge/mesh.frag`

Screen-space GE mesh path:

| Location | Attribute | Format |
|---|---|---|
| 0 | position.xy (pixels) | `R32G32_SFLOAT` |
| 1 | depth (0..65535) | `R32_SFLOAT` |
| 2 | texture uv | `R32G32_SFLOAT` |
| 3 | colour RGBA (0..1) | `R32G32B32A32_SFLOAT` |
| 4 | clip w | `R32_SFLOAT` |
| 5 | texture q | `R32_SFLOAT` |

Push constant: `vec2 viewport` (8 bytes). The vertex shader converts pixels to
NDC, multiplies by clip `w`, and outputs `vec3(s, t, q)`; the fragment shader
samples `st/q` and modulates by the vertex colour. The sampler is selected by
the texture cache (nearest/linear, clamp/repeat).

## Post shaders

`post/fullscreen.vert` emits a full-screen triangle and `uv` in [0,2].

| Shader | Pass | Push constants |
|---|---|---|
| `integer_scale.frag` | nearest integer scale | source/output size |
| `fsr_lite.frag` | bilinear + contrast sharpening | source/output size, sharpness |
| `scanlines.frag` | darken every `period`-th row | source/output size, strength, period |
| `lcd_grid.frag` | darken a `period` grid | source/output size, strength, period |

The CPU implementation in `runtime/post/post.cpp` defines the current pinned
hashes; the GLSL passes mirror its parameters. They are not yet recorded into
the Vulkan present path (named gap in `docs/RENDERER.md`).

## Preset JSON

```json
{
  "name": "crt",
  "passes": [
    {"type": "integer_scale", "factor": 2},
    {"type": "scanlines", "strength": 0.25, "period": 2}
  ]
}
```

Supported types: `integer_scale` (`factor` 1-8), `bilinear` (`scale` 1-8),
`fsr_lite` (`scale`, optional `sharpness` 0-1), `scanlines` and `lcd_grid`
(`strength` 0-1, optional `period` 1-16). Unknown types, malformed JSON,
missing `passes`, and out-of-range values fail with named errors; an invalid
preset is never partially applied.

## Rules

- No game-derived screenshots, textures or text in fixtures or shader tests.
- No generated SPIR-V in git; regenerate from the user's build.
- A shader semantic change needs a new pinned hash or a mutation that fails the
  expected fixture.
