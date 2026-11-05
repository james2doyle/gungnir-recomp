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

## Presentation and display layout (T16)

Presentation chooses where the 480x272 guest frame lands on the display; it
never changes what the guest renders and never enters a snapshot.

Current pieces:

- `present_rect` (`vulkan_backend.cpp:377`) turns the policy and the two
  extents into the rectangle drawn into, `fit_viewport` (`:358`) is the fit
  case, `clamp_scissor_to_target` (`:415`) offsets a guest-scaled scissor onto
  the surface and clamps it, and `scale_scissor_to_extent` (`:427`) does the
  guest-to-rectangle mapping. The present path applies them at `:2158`.

Policies (`DisplayLayout`, host state via `--es display_layout` and the
`Screen fill` settings row):

| Policy | Rect | Costs |
|---|---|---|
| `fit` (default) | aspect-preserving, centred | bars at the sides on a wide phone (today's behaviour) |
| `stretch` | the whole target | non-uniform: on 19.5:9 that is ~23% horizontal magnification or ~15% vertical squash |
| `crop` | uniform scale until the target is covered | loses ~20% of the frame height (~10% top and bottom); the scissor is clamped to the surface |

`stretch` and `crop` are presentation-only compromises: the scene is not
rendered wider, so crop cuts real picture and stretch distorts it. Filling the
display without either is the widescreen work below.

Invariants:

- A Vulkan scissor origin must not be negative (`VUID-vkCmdSetScissor-x-00595`).
  Crop presents with a negative origin, so the scissor is clamped after
  offsetting and rects that fall entirely off the surface are dropped.
- An unknown policy name is a named error (`unknown_display_layout: <text>`) and
  the layout stays as it was; it is never guessed.
- Presentation is absent from the snapshot config identity: switching the
  policy must not invalidate a save state.
- The touch overlay stays in window space. It has no guest-pixel mapping (its
  controls are placed in normalized window coordinates), so anchoring the
  controls to the presented image would move them off the black bars and break
  existing layouts for no input gain; `fit` therefore keeps its controls over
  the bars, as before.

### Adaptive widescreen (T16a)

`--es widescreen adaptive|off|16:9|21:9|32:9` (default `adaptive`), modelled on
the adaptive view in `mstan/gbarecomp`: the render frame follows the window
instead of a fixed 480x272.

- `adaptive` measures the window aspect and widens to it (`k = window_aspect /
  1.7647`; 3063x1335 -> k = 1.3001, virtual 624x272), so presentation with
  `fit` fills the display exactly. The aspect caps (`16:9`, `21:9`, `32:9`)
  bound `k` for windows wider than the content should stretch to; `off` is the
  native frame.
- The scene is widened, not zoomed: the horizontal row of the
  world-view-projection matrix shrinks by `k` and the projection step grows by
  `k`, so the picture keeps its pixel size and the extra columns show world the
  native frustum cut off.
- Screen-space (`through`) draws are split by coverage. A draw whose bounds
  cover the guest frame is a full-frame effect (backdrop, fade, vignette) and
  **stretches with the scene**, so the margins are never empty; anything else
  is HUD and keeps its pixel size, only recentred by the margin. Scissors follow
  the same split (scene and full-frame scissors scale, HUD scissors recentre).
- There is deliberately no presentation-layout settings row while `adaptive`
  fills the display: `fit`, `stretch` and `crop` all produce the same rectangle
  on a frame that already matches the display aspect, so offering the row would
  be a control that does nothing. The policies remain available through
  `--es display_layout` for QA and for builds with widescreen off.
- Verified on the phone: `widescreen mode=adaptive scale=1.3001 virtual=624x272`
  with `present_rect ... rect=0,0 3063x1335` — the whole target, HUD centred and
  undistorted, the battle backdrop continuous to both edges.

Known limits: the guest culls its own 3D against the unwidened camera, so
geometry outside the native frustum is never submitted and the widened columns
show the (stretched) backdrop where the scene has nothing (a per-title camera
hook would be the only recovery). Scenes that render into a 480-wide VRAM
target keep the 480 mapping and are a counted gap.

### Measuring the gap

The limitation above is a **measured gap**, not a description. At present time
we compute, from the draw bounds already available, what fraction of the
widened frame's columns carry submitted guest geometry and how many columns
show only the stretched full-frame 2D backdrop (or nothing). The measure is
bounds-only: one pass over the draws and one pass over at most `virtual_width`
column flags, no extra render pass and no per-pixel work. A draw is geometry
unless it is a through (screen-space) draw covering the guest frame — those
are the stretched backdrop / full-screen effects; a small HUD panel is
geometry. The policy is pinned by `tests/ge/widescreen_coverage_tests.cpp`.

Every log tick reports
`widescreen coverage=NN% backdrop_only_columns=N hook=<on|off|absent>
samples=N scale=... columns=N`, averaged over the presents in that tick. A
10-point step in the percentage (a scene change) also produces a one-shot line
so a scene regression is visible in a bounded run without waiting for the
tick. `hook=absent` is the honest current state: no per-title camera hook is
applied yet. Record the number before and after a hook lands (see
`docs/QA.md`) — a hook that widens the guest's culling should raise coverage
and reduce backdrop-only columns. The number is a bounds approximation: it
counts columns touched by any geometry draw, not pixels, so it is an upper
bound on visible geometry.

## Frame interpolation (T17)

The guest title is a locked 20 Hz producer: it flips, sleeps two ~8 ms guest
delays, then waits two vblanks (`1 + 2 = 3` vblanks, 16.683 ms each). That was
measured, not assumed; see `docs/qa/2026-11-03-m2102k1ac-interpolation.md`.
Frame interpolation runs on top of that rate and never changes what the guest
computes.

- **Identity.** `draw_key` covers primitive, vertex count, texture key, target
  format and the GE through flag. The target buffer address is deliberately
  excluded: the game alternates two display buffers, so an address-based key
  matches nothing between frames.
- **Partners.** Draws of one identity form a bucket, and a partner is the
  previous draw of that bucket *nearest the current one*, not the draw at the
  same arrival index. Arrival order inside a bucket is not stable: a cursor
  highlight, an animation frame or a culled sprite adds or drops a draw and
  shifts every later one, so index matching blends each quad of a character
  toward a neighbouring quad and the sprite shakes apart. One exception, counted
  as `oversize`: buckets past `kMaxBucketScan` (64 draws) keep arrival order,
  because repetitive scenery -- the battle map's 546 tile draws of one texture --
  is equidistant from its own previous position and its neighbour's, so position
  matching slides the whole ground by one tile whenever the pan moves further
  than the tile spacing.
- **Candidates.** Every draw with a triangle. Screen-space (through-mode) draws
  are *not* excluded: Gungnir projects its battle unit sprites on the CPU and
  submits them that way, at fixed 30x47 sizes, moving with the camera. Excluding
  the class left the units stepping at the guest rate while the map behind them
  was smoothed, which is the sprite shake that was reported. A draw whose
  identity changed between frames has no partner and is shown as produced, so
  HUD text and glyphs do not smear; static HUD is identical in both frames, so
  blending it changes nothing.
- **Reference check.** `BasedYuki/the-3rd-birthday-recomp` skips through draws
  (`ge_gpu.c`: `if (d->through || ...) s_fi_draw = -1`) and matches by
  key + occurrence, with a flat 0.3-of-a-screen cut. Both rules are right for a
  title whose 2D layer is HUD and whose motion is matrix-driven; here the units
  are CPU-projected sprites and the camera pans, so the class exclusion and the
  flat distance both produced artefacts. Its per-draw previous matrices come from
  the recompiled game itself, which we do not have without a per-title hook.
- **Motion.** Matched draws move to `t` of the way from the previous frame, by
  **perspective-correct interpolation**: because perspective division is not
  affine, a projected point is blended as `(position * w)` and `w`, then divided
  (`inv_w` becomes the blended depth's reciprocal). Affine blending of screen x/y
  while blending `inv_w` separately leaves a position that corresponds to no 3D
  point, which is what makes an inclined or receding mesh swim between frames.
  Colour, texture coordinates, **depth** and state stay the frame's own, and so
  does the shape of a draw that rotated or scaled between frames: interpolating
  its vertices independently collapses it (blending a quad with its rotated self
  shrinks it, and near 180 degrees it becomes a line) — measured 11-29 collapsing
  triangles per frame pair, all effect geometry, which is why particles and
  banners tore while everything else moved cleanly. Such a pair is translated
  rigidly instead, so motion stays smooth and the shape is the frame's own. The
  test is the collapse itself, not only a distance: a one-pixel-tall sliver
  inverts when each corner moves about a pixel, so a pair whose vertex-wise blend
  would halve or invert any triangle takes the same path. Depth is an ordering key rather than a position, and
  coplanar geometry (a wall and its trim, a tile and its decal) relies on exact
  equality under the GE's GEQUAL test: on the battle snapshot 432 overlapping
  pairs changed depth-equality class in the blend while agreeing frame to frame,
  which is how thin walls shimmer. This is the Chinatown Wars vertex-midpoint method; The 3rd
  Birthday's matrix reprojection needs per-draw matrices we do not keep and
  stays the fallback if midpoint looks wrong on skinned content.
- **Cuts.** Rejection is judged against the frame's own motion, because screen
  motion scales with depth: in a pan the foreground legitimately moves several
  times faster than the map behind it (measured: a 14 px median with foreground
  elements at 169 px). A pair is unrelated when it moves more than
  `kMotionSlack` times the frame's median motion plus `kCutScreenFraction` (0.2 of
  the screen, unchanged from the original design), and a frame is a camera cut
  when its median motion exceeds `kFrameCutFraction` (half a screen in one guest
  interval) and nothing in it is blended. Two stricter rules were tried and
  withdrawn because they snapped things that used to blend: judging a draw
  against its own identity bucket (a door opening in an otherwise static bucket is
  a fast member, not a mistake) and lowering the still-scene floor to 0.1 (a door
  60 px into its swing became a "teleport"). Rejection may only be *relaxed* from
  the original behaviour, never tightened. A flat per-draw limit was the earlier design and it made every fast
  level element jump once per present while its neighbours glided -- first
  reported as sprites shaking, then as level elements flickering. In a still
  frame the median is zero, so the 0.2-of-a-screen margin still snaps a cursor or
  a selection highlight that teleports.
- **Schedule.** `PresentSchedule` starts a display window per guest frame,
  presents one in-between per display refresh (`t = elapsed / window`, where the
  window is the previous guest interval), and shows the real frame at the end
  of the window rather than extrapolating. That costs one guest interval of
  display latency by construction; it never accumulates.
- **Never in a snapshot.** The history is derived and lives only in the host;
  snapshot load, reset to title and background resume clear it, so no in-between
  bridges a discontinuity. The option is host state, not part of the snapshot
  identity.

Limits, named rather than silent: the presented rate is bounded by the cost of
one present. At the battle scene (2834 draws) a present costs about 10 ms, so
the ceiling is near 100 presents/s regardless of a 120 Hz panel; `skipped_ticks`
in the periodic log reports the shortfall. `tools/interpolation_report.py <dump>` reads a frame dump from the device
(`--es dump_draws <path> --el dump_after_ms <ms> --el dump_frames N`) and says what
a pass would do with the frames the game really produced: bucket sizes, frames
whose arrival order broke, collapsing triangles, and what the shape-change answer
prevented (`--vertex-only`). The `interpolation_pairs` line reports
what one pass did: `reassigned` draws whose partner came from position rather
than arrival order, `displaced` of those that arrival order would have dragged
visibly, `oversize`/`oversize_draws` buckets on the arrival-order fallback and
how many draws they hold, and `largest_bucket`.

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

## Bounded GE trace diagnostics (host-side, opt-in)

These are research diagnostics for a bounded host boot (`boot_runner`), all
off unless the named env var is set and all capped by a `*_LIMIT` env var
(default 4096 lines). None changes what is presented, and no log is committed.

| Env var | Logs | Reader |
|---|---|---|
| `GUNGNIR_TRACE_MEDIA` | GE list enqueues and display flips | — |
| `GUNGNIR_TRACE_CAMERA` | GE projection (0x3E/0x3F) and world/view/bone (0x3A–0x3D, 0x2A/0x2B) matrix commands as `[camera_projection]` / `[camera_transform]` | `tools/camera_trace_report.py`, `tools/camera_transform_report.py` |
| `GUNGNIR_TRACE_VERTEX_SOURCE` | GE vertex/index address commands (0x01/0x02) as `[vertex_source]` with the resolved address | — |
| `PSPRECOMP_WATCH_WRITE` (+ `_SIZE`) | every guest store to an address range, with the guest dispatch PC | — |
| `--es dump_draws` / `--es dump_presents` | bounded guest-frame / presented-frame dumps | `tools/interpolation_report.py` |

`GUNGNIR_TRACE_VERTEX_SOURCE` was used to establish that the guest writes
already-projected vertices into a small set of shared scratch buffers (16
distinct addresses per frame list), so neither the vertex address nor the
display-list command PC is a per-object identity. See `docs/BRINGUP.md` for
the measured conclusion and the named gap.
