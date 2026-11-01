# RENDERER — GE pipeline, Vulkan backend and parity gates

Status: T7a–T7h implemented. This document describes the Linux renderer,
not the Android shell (T9). The remaining named gaps are recorded below.

## Pipeline

```
guest display list
  -> ge::DisplayList             command decode, BASE/OFFSET/ORIGIN, CALL/RET,
                                 stall, END/SIGNAL/FINISH, VTYPE/PRIM packets
  -> ge::VertexPipeline          vertex formats, morph/skinning, world/view/
                                 projection, texture matrix, lighting, clipping
  -> ge::SoftwareRasterizer      CPU parity oracle (depth16, stencil8, blend,
                                 scissor, alpha test, texture sampling)
  -> VulkanBackend               Vulkan 1.1 mesh draw path (pipeline cache,
                                 texture descriptors, depth/blend/scissor)
  -> post::apply / GLSL post     preset chain over the composited RGBA8 image
```

The HLE GE queue uses `DisplayList` directly. It stops at stall addresses,
resumes through `sceGeListUpdateStallAddr`, carries SIGNAL/FINISH callbacks and
logs a named decoder failure instead of completing a broken list silently.

## Vulkan draw path

`VulkanBackend` creates a Vulkan 1.1 instance/device/graphics queue, an
offscreen RGBA8 + depth16 target for headless readback, and a windowed
swapchain over a caller-supplied surface (`Config::create_surface`). The mesh
path accepts screen-space vertices with clip `w` and texture `q`, uploads
vertex/index buffers, and binds one descriptor set per texture.

Pipeline cache key: blend enable/source/destination/equation, depth
test/write/function, cull mode and front-face winding. Repeated state reuses
the pipeline; changing state creates one new pipeline. Tests assert both the
reuse and the increment.

Texture cache key: format, swizzle, width, height, `bufw`, clamp/filter state,
CLUT format and all texel/CLUT bytes. Uploading a texture allocates a
descriptor set; invalidation frees the descriptor and destroys the image.

## Render-to-texture feedback

`TextureRangeIndex` tracks guest-address ranges that back cached textures. A
render into an overlapping range invalidates the software cache and calls
`VulkanBackend::invalidate_texture` through the same range walk. The CLUT
re-upload fixture changes the palette, invalidates the range, and requires the
next Vulkan frame to match the changed software frame; removing invalidation
fails that test.

## Fixtures and tolerance

| Gate | Fixtures | Tolerance |
|---|---|---|
| TC-7.1 | triangles, strips, fans, sprites, points, lines, all colour formats, CLUT4/8/16/32, blend, scissor, depth, alpha test, stencil-lite | hand-computed pixels |
| TC-7.2 | solid colour, depth order, source-alpha blend, scissor, CLUT texture, invalidated CLUT texture | software vs Vulkan: <= 1 LSB per 8-bit channel |
| TC-7.3 | default/integer/scanline/LCD-grid/FSR-lite presets | pinned readback hashes |
| TC-7.4 | texture-range write hitting/not hitting a texture, CLUT re-upload | exact invalidation count and pixel parity |

Synthetic fixtures only; no game frames, screenshots or assets are committed.
The consolidated gate is `tests/test_ge_suite.py`.

## Mutation evidence

These semantic mutations were applied locally and the named suite failed:

| Mutation | Failing check |
|---|---|
| T7a: BASE argument shifted the wrong way | `BASE takes high byte from bits 16-23` |
| T7b: world dropped from world-view-projection | `view * world adds both translations` |
| T7c: `Less` depth comparison inverted | `nearer fragment wins` |
| T7d: CLUT bytes omitted from the content key | `CLUT bytes change the content key` |
| T7e: vertex colours zeroed | `headless triangle wrote colour pixels` |
| T7f: texture invalidation made a no-op | `invalidated texture re-uploads and matches software` |
| T7g: integer-scale `dy` offset dropped | `integer scale replicates each texel exactly` |

## Validation and named gaps

The host used for T7 has the Vulkan loader and a Radeon ICD but no
`VK_LAYER_KHRONOS_validation`. The gate requests validation explicitly and
accepts the named `validation_layer_unavailable` error; a validation-layer
clean run is therefore not claimed.

Unimplemented or unexercised GE paths, counted as named gaps:

1. No full GE register/state machine maps a real display list's matrices,
   lights, texture addresses and `PRIM` packets into `VertexPipeline` and
   `VulkanBackend` yet. The queue decodes and counts draws; fixtures drive the
   backend with screen-space vertices.
2. The Vulkan path accepts indexed triangle lists. Lines, points and sprites
   are supported only by the software oracle.
3. Texture formats 0-7 are decoded; PSP DXT formats 8-10 are not.
4. The post chain is the CPU oracle. GLSL post shaders compile, but are not
   yet recorded into the Vulkan present path.
5. Windowed presentation is tested with a hidden SDL3 window on desktop and
   with SDL3's Android Vulkan surface on the emulator and phone (T9b); the
   software oracle remains the parity path.
6. No validation-layer binary is available in this environment.
7. Blend approximations: doubled-alpha codes 6-9 map to the single variant
   on Vulkan (no doubled Vulkan factor exists); when both sides are `FIX`
   with different `BLENDFIXEDA/B` registers the source register wins on both
   backends; `FIX` alpha uses 1.0. Covered by `blend_factor_table`,
   software `blending` and the Vulkan parity cases.

## M3 evidence

- Bounded Linux boot: `docs/HLE.md` — module init complete, first flip,
  deterministic 200,000-dispatch report.
- GE fixtures: `tests/test_ge_suite.py` — TC-7.1–TC-7.4 over synthetic
  fixtures, 75-test repository suite green, `repository_check` PASS (211 files).
- T7 closure: `data/milestones.json` M3 entry. Android presentation landed in
  T9b; device QA and the title-screen milestone remain M4/T13 work.
