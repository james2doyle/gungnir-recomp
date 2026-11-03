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

## Presentation, widescreen and display layout (T16 — PLANNED)

Not implemented yet. This records the renderer-side contract the T16 design
must keep; the authoritative work items are the T16 dex tasks.

Current behaviour: `fit_viewport` (`vulkan_backend.cpp:358`) fits the
480x272 guest frame into the swapchain preserving aspect, and
`scale_scissor_to_extent` (`:377`) maps guest scissor coordinates into the
fitted rect; both run at `:2109`/`:2122`.

Planned settings, deliberately separate:

- `widescreen` (off / display aspect / 16:9 cap) changes the projection and
  enters the snapshot config identity.
- `display_layout` (fit / stretch / crop) changes only the presentation
  rectangle and never enters the snapshot identity.

Invariants:

- 480x272 is 1.765:1, already almost 16:9 (16:9 is 483.6 px at 272 height).
  Widening to 16:9 is a 0.7% no-op; the real mode is the display aspect
  (19.5:9 -> 604x272, `k = 1.26`).
- Only perspective draws widen. `through` draws and draws whose projection
  w-row is zero stay on the 480 mapping. That 2D/3D tag does not exist on
  `DrawState`/`VulkanDrawState` today and must be added and tested.
- A 2D draw covering the full frame is a full-screen effect (fade, vignette,
  letterbox bar) and stretches with the scene, otherwise fades leave unfaded
  strips at the sides.
- The shader normalizes with the pushed viewport (`assets/shaders/ge/mesh.vert:31`),
  so the effective virtual width must be pushed and every vertex shifted by
  the same origin; the 480-wide HUD then lands centred without distortion.
- Scissors follow the draw class (widened `x*k`, 2D `x+off`). Crop must
  clamp to `[0, extent]`: Vulkan requires non-negative scissor offsets
  (`VUID-vkCmdSetScissor-x-00595`), and crop has a negative origin.
- Only display-buffer draws widen. A scene that renders into a 480-wide VRAM
  target keeps the 480 mapping and is a counted gap, never a silent no-op.
- Guest CPU culling still uses the unwidened camera, so geometry outside the
  original frustum stays missing. That is not a renderer bug and cannot be
  recovered host-side (only per-title camera hooks would).

Numbers on 19.5:9: stretch is ~23% horizontal magnification (or ~15%
vertical squash); crop loses ~20% of the frame height (~10% top and bottom).
FMV/PSMF is not implemented (every `sceMpeg` entry point is a named gap), so
pillarboxing is unit-tested with a full-screen 2D quad only and device
acceptance stays blocked on movie playback.

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
