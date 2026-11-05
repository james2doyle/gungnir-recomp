# 2026-11-04 — Widescreen coverage + per-title host profile (device)

Date: 2026-11-04. Build: `5fb2b2f` + `-dirty` (uncommitted host-profile,
camera-trace and coverage work; the tree is deliberately dirty pending the
user QA gate). APK SHA-256:
`9999f508cce15a25ba5befabfe9bd2d7e5bc7b8a6040edda39777b9a3a406779`.

Screenshot SHA-256:
`28075f9ee8b803f88ec7a8c4d3862692b2dcb90f23bc8824a9c9190972e5d4bd`
(`build/qa/widescreen-coverage/screen.png`, gitignored; battle deployment
scene, "Choose where to deploy", 3D scene continuous to both edges, HUD
centred).

## Device

| Field | Value |
|---|---|
| Model | Xiaomi M2102K1AC |
| Serial | 30c1f958 |
| Android | (as recorded in previous cells) |
| SoC/GPU | Adreno (arm64-v8a) |
| ABI | arm64-v8a |
| Framebuffer | 3063x1335 |
| Widescreen | `adaptive scale=1.3001 virtual=624x272` |

## Steps

1. Build the debug APK offline for `arm64-v8a` with the local generated corpus.
2. Install `-r` on `30c1f958`; force-stop; clear logcat.
3. Push a sample profile through `run-as` stdin (clearly fake placeholders:
   `cam_projection=0`, `cam_setup=0`, `widescreen_fov_scale=1.0000`). Real
   values stay in gitignored per-title local storage; no dump identity here.
4. Boot with the battle snapshot via `--es snapshot_in`.
5. Read the `profile`, `widescreen coverage`, `present_time` and `first frame`
   lines over ~12 s (two 5 s log ticks).
6. Replace the profile with an unknown key (`turboboost = 1`), reboot, verify
   refusal; then remove the profile, reboot, verify the absent case and that
   the same snapshot still loads.

## Expected / actual

| Step | Expected | Actual | Pass |
|---|---|---|---|
| 3-4 | Every applied entry logged as `profile key=value`; `profile applied=N` | `profile cam_projection=0`, `cam_setup=0`, `widescreen_fov_scale=1.000000`, `profile applied=3` | PASS |
| 4 | Snapshot loads with a profile present | `session_start` + `first frame 3063x1335` | PASS |
| 5 | Stable coverage across two ticks; one-shot line on scene change; no present-time regression | one-shot 0% → 97% at boot; tick1 `coverage=96% backdrop_only_columns=21 samples=195`, tick2 `97% / 18 / 201`; `avg_us` 12193 → 11836 (within noise) | PASS |
| 5 | Number matches the screenshot | screenshot shows 3D scene continuous to both edges, HUD centred; 18-21 of 624 columns backdrop-only is the extreme margin | PASS |
| 6 | Invalid profile refused with a named error, no entries applied | `E gungnir: profile_invalid: unknown profile key 'turboboost'` | PASS |
| 6 | Absent profile changes nothing and does not invalidate the snapshot | `profile absent`, same snapshot loads, coverage line still emitted | PASS |

Counters: `present_time frames=396 total_us=4687343 avg_us=11836`; no crashes
in the log buffer; `widescreen mode=adaptive scale=1.3001 virtual=624x272`.

## Interpretation (honest boundary)

`hook=absent` is the current state — no per-title camera hook is applied. The
measure counts presented columns touched by *any* geometry draw; the host
widens 3D geometry with the projection step, so most columns are touched and
coverage is high (96-97%). The remaining 18-21 columns are the extreme margins
where nothing was submitted. This is a **bounds-only upper bound on visible
geometry**, not a pixel count, and it does not by itself prove the guest's cull
gap (which the camera trace measured separately via scissor `[0,0,480,272]`).
The value of this line is that it is now *measured and stable*: record it
before and after a hook lands, and a hook that widens the guest's culling
should change it.

## Sign-off

Signed: per-title host profile boot wiring (applied logging, refuse-invalid,
absent case, snapshot purity) and widescreen coverage diagnostic (stable,
screenshot-matched, no present-time regression) on `30c1f958`. Automated host
gates are green; this is a manual device cell. Scope: one cold boot per case
(profile present / invalid / absent) — the three-consecutive-cold-boot sprint
gate for the milestone is a separate, later cell.
