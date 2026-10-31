# liblinkernsbypass provenance

- Upstream: https://github.com/bylaws/liblinkernsbypass
- Pinned commit: `aa3975893d83ef1bc84c321ec60c65fbf1287887`
- Archive: `https://github.com/bylaws/liblinkernsbypass/archive/aa3975893d83ef1bc84c321ec60c65fbf1287887.tar.gz`
- Archive SHA-256: `da1128c8aa771c4d24766b53a47822d0717baa5c536ca8491220402942b80638`
- License: BSD-2-Clause (`LICENSE` in this directory, copied verbatim)
- Disposition: **fetch-only**, transitive dependency of libadrenotools. No
  upstream source or binary is committed here; only the license and this note
  are tracked.

## Why it is pinned separately

libadrenotools declares `lib/linkernsbypass` as a git submodule pointing at
this repository. GitHub commit archives do not include submodule contents, so
the Android build must fetch this archive separately and unpack it into
`libadrenotools/lib/linkernsbypass/` before configuring libadrenotools. Pinning
it independently keeps both archives content-addressed and verifiable.

## What it provides

`android_linker_ns.cpp` creates a private Android linker namespace so the
custom ICD can be loaded outside the system Vulkan namespace;
`elf_soname_patcher.cpp` rewrites the ICD's DT_SONAME so the linker accepts it.
The pinned revision includes the `getpagesize()` fix for 16 KiB-page arm64
devices.

## Local patches

None.

## References checked

`gh search code "adrenotools_open_libvulkan" --extension cpp` and
`gh search code "linkernsbypass" --extension cpp` identified the call sites and
the submodule relationship. The submodule commit was read from the upstream
gitlink at the pinned libadrenotools commit.
