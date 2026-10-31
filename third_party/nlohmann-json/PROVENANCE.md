# nlohmann/json provenance

- Upstream: https://github.com/nlohmann/json
- Release: `v3.12.0`
- Commit: `55f93686c01528224f448c19128836e7df245f72`
- Header asset: `https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp`
- Header SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`
- Include archive: `https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip`
- Include archive SHA-256: `b8cb0ef2dd7f57f18933997c9934bb1fa962594f701cd5a8d3c2c80541559372`
- License: MIT (`LICENSE` in this directory, copied verbatim from the
  upstream `LICENSE.MIT` at the pinned commit)
- Disposition: vendored single-header release asset, byte-identical.

## What is used

`nlohmann/json.hpp` only, for strict parsing of the untrusted `meta.json` in a driver
package. The parser is configured to reject exceptions and the inspector maps
parse/type failures to named errors; we never hand-write JSON parsing.

## Why vendored instead of fetched

The header is self-contained and needed by both the Android build and the host
tests. Vendoring it keeps offline host builds simple; the exact commit and the
verified vendored-tree digest are in `dependencies.json`.

## Local patches

None.
