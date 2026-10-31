# SDL3 — provenance

SDL3 is a **fetch-only** dependency for the Android shell. No SDL3 source or
binary is committed; `cmake/Deps.cmake` downloads the pinned release tarball
into the gitignored `build/android-deps/` tree and verifies its SHA-256 before
extracting it. `tools/deps.py` verifies the same pin when the archive is
materialized.

- Upstream: <https://github.com/libsdl-org/SDL>
- Release: `release-3.4.18` (`SDL3-3.4.18.tar.gz`)
- License: Zlib (`third_party/sdl3/LICENSE.txt`)
- Use: SDL3 Activity/Java layer, Android lifecycle, Vulkan surface and input;
  linked statically into `libmain.so` so the APK carries no `libSDL3.so`.

The Gradle build reads SDL's Java sources from the fetched tree; they are
compiled into the APK but never copied into this repository.
