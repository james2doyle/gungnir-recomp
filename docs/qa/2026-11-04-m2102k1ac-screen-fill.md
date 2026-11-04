# QA — screen-fill presentation cell (T16b)

Date: 2026-11-04
Build: the tree carrying `present_rect` / `clamp_scissor_to_target` and the
`Screen fill` row; committed right after this cell. No `-dirty` renderer code
was in the artifact.
APK: `android/app/build/outputs/apk/debug/app-debug.apk` (debug, `arm64-v8a`,
AOT corpus `build/generated`)
APK SHA-256: `eac8f29272993bee98d191f80394609f7f3862fa93e3de6f1b3e22b6fac33405`

Sign-off: **PASS — fit keeps today's behaviour, stretch fills the display and
distorts, crop fills it uniformly and clips the top and bottom, the row cycles
through all three with a toast, the choice persists and an unknown name is a
named error.** Crop's cost is recorded here: it is a zoom, so parts of the
HUD go off-screen. Filling the display without distortion or clipping is the
widescreen widening work, not this task.

## Device

| Field | Value |
|---|---|
| Model | M2102K1AC |
| Android | 14 (SDK 34) |
| SoC platform | SM8350 / lahaina |
| GPU | Adreno (system driver) |
| Display | 3200x1440 panel, app framebuffer 3063x1335 (2.294:1), 120 Hz |
| ABI built | `arm64-v8a` |
| Serial | `30c1f958` |

## Steps

1. Build and install the APK, push the battle snapshot, launch with
   `--es snapshot_in <path> --es display_layout <fit|stretch|crop>`.
2. For each policy: confirm `display_layout=<name>` in the log, capture a
   screenshot after the first frames, and compare the presented rectangle.
3. Open the settings overlay, tap `Screen fill` three times, and confirm the
   label and the toast follow and the scene changes underneath.
4. Toggle the policy and relaunch to confirm the choice persists.
5. Check for crashes and ANR across the cell.

## Expected and actual

| Step | Expected | Actual |
|---|---|---|
| `fit` | Pillar-boxed, correct aspect | Pass: 2356x1335 at x=353 — 22.9% of the width unused in two bars |
| `stretch` | Fills the target, non-uniform | Pass: 3063x1335; the scene is 30% wider than a correct aspect |
| `crop` | Uniform fill, top/bottom clipped | Pass: 3063x1736 at y=-200 — 23% of the image height off-screen (11.5% each side); the party bar and the unit banner are visibly cut |
| Row cycle | Fit → Stretch → Crop with a toast | Pass: `Screen fill: Stretch` / toast `Screen fill: stretch`, then `Crop`, then `Fit` |
| Persistence | Survives a restart | Pass: the relaunched session keeps the selected policy |
| Unknown name | Named error, layout unchanged | Covered on the host (`unknown_display_layout: zoom`); not re-run on device |
| Input | No regression | Pass: touch overlay and settings navigation unchanged; presentation does not alter input mapping |
| Crashes | None | Pass |

## Geometry (app framebuffer 3063x1335, guest 480x272)

| Policy | Rect | Consequence |
|---|---|---|
| fit | 2356x1335 @ (353,0) | bars 353 px each side |
| stretch | 3063x1335 @ (0,0) | 1.30x horizontal magnification |
| crop | 3063x1736 @ (0,-200) | 23% of the height clipped |

## Screenshot SHA-256

Images stay under gitignored `build/qa/`.

- `fit`: `8c9419aedeb1db64175227b571f3f79cfc0f4f470d95595d6ffd61c500225e53`
- `stretch`: `80606b6ac33262fe47074a37e5a353bf8c70b64b04d95f86702ee6d5c49b7e70`
- `crop`: `6200d81b7d83e46dd5c0d69974cbc74d7b805ac29221d9a1bddea73cfb25b085`
- settings row before cycling: `ef10c35109649d1be9f09da93e2bc43324bc27afbf3a1761bbe4f7a305084635`
- settings row after one tap: `4e67d97269413db29267e9eea01f782d4cd3bb2604d44e960564af873e7f1c77`

## Findings

- **Crop is a zoom, not a wider view.** Filling a 2.294:1 display with a
  1.765:1 frame by scaling uniformly means 23% of the image leaves the screen;
  the party bar at the bottom and the unit banner at the top are cut. That is
  the documented trade-off, not a defect, and it is why `fit` stays the
  default.
- **The real answer is widening the projection.** `stretch` distorts and `crop`
  clips because the guest still renders 480x272. Widening the projection for
  perspective draws (the widescreen work) is what fills the display with the
  whole frame visible and the HUD undistorted.
- **Scissor clamping is load-bearing.** Crop presents with a negative viewport
  origin, and a Vulkan scissor origin must not be negative; the scissor path
  now clamps to the surface after offsetting and drops rects that fall off it
  (unit-tested, and the mutation that transposes the stretch mapping fails the
  geometry test).

## Notes

- Presentation never enters a snapshot: switching the policy does not
  invalidate a save state (unit-guarded).
- No game frames, dump hashes or generated content are stored with this
  record; screenshots stay under the gitignored `build/qa/` tree.
