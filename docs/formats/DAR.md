# DAR — Gungnir asset archives

Status: format facts for T8/T12 and the `tools/dar.py` reader/rewriter. No DAR
bytes or extracted assets are committed. This document is written from the T3
intake knowledge and the user's local files at layout level only.

## Container

Gungnir stores assets in `PSP_GAME/USRDIR/{INI,DATA,VOICE}.DAR` (plus
`INSDIR/DATA.DAR`). All integers are little-endian.

```
0x00  u32  nfiles
0x04  u32  data_start
0x08  u32  names_off
0x0C  u32  table_off
      names: NUL-terminated ASCII paths, addressed by name_off
      table: nfiles entries of 16 bytes
      data:  entry payloads, zlib-compressed or stored
```

Entry:

| Offset | Size | Field |
|---|---|---|
| +0x00 | u32 | name offset |
| +0x04 | u32 | stored size |
| +0x08 | u32 | decompressed size |
| +0x0C | u32 | data offset |

`decompressed_size > stored_size` means the payload is a zlib stream of
`stored_size` bytes. **`stored_size == 0` means stored uncompressed**, with
the real length in `decompressed_size`; it does not mean empty. This is the
trap that zeroed 272 sound effects in a previous translation project.

## In-place rewrite rules

DAR entries contain asset files whose internal pointers/jumps are absolute
(script `.bin` files are the extreme case). A rewrite therefore follows these
rules:

1. **Do not change file sizes** unless every internal pointer/jump is
   remapped. Script `.bin` files must keep their size.
2. Untouched entries keep their original `data_off`; internal absolute
   references remain valid.
3. A recompressed blob must fit its original slot. Vary zlib parameters to
   squeeze it; if it does not fit, stop with a named error rather than moving
   data.
4. The safest rewrite is true in-place plus zero-fill of the unused tail.
   Rebuilding the archive in sequence moves every `data_off` and is not
   acceptable for files with internal references.
5. When a DAR is embedded in an ISO 9660 image, the directory record carries
   the size twice — a little-endian field and a big-endian copy four bytes
   later. Patch both or the mount breaks.
6. Every writer runs the identity gate first: unpacking and repacking the
   original must reproduce the input byte-for-byte before any new payload is
   written.

## Compression

Payloads use zlib. `stored_size == 0` is stored data with the length in
`decompressed_size`; `stored_size == decompressed_size` is stored data;
`decompressed_size > stored_size` is a zlib stream. A payload that fails to
inflate, a `data_off` outside the archive, or a name offset without a NUL
terminator is an error with a named code, never a silent skip.

Names are relative paths (for example `dir/name.ext`) and may contain `/`. Absolute
paths, `..`, `.`, empty components and backslashes are rejected on read and
extract, so an archive cannot escape the extraction directory. Extraction
creates parent directories.

A zlib slot can only carry a replacement whose uncompressed size is larger
than the slot; otherwise the format's only compressed/stored discriminator
(`decompressed_size > stored_size`) would classify it as stored and the
reader would see the compressed bytes. `dar.py` rejects that with
`DAR_COMPRESSED_SLOT_TOO_SMALL` rather than writing an unreadable entry.

## Tool

```sh
python3 tools/dar.py list ARCHIVE
python3 tools/dar.py info ARCHIVE
python3 tools/dar.py extract ARCHIVE --out DIR
python3 tools/dar.py repack ARCHIVE --from DIR
```

`extract` refuses to overwrite an existing file unless `--force`. `repack`
requires the extracted source directory, compares every payload with the
original, leaves unchanged entries untouched and recompresses only changed
zlib entries (levels 9..1 until the blob fits). It patches payload slots and
the entry's `decompressed_size` in place; every `data_off` and the archive
size stay fixed, and unused compressed tail bytes are zero-filled. A missing
source file, a size change in a stored slot, an oversized zlib blob or a
failed write is a named error. The tool writes through a sibling temporary
file and `os.replace`, so a failed rewrite does not leave a torn archive.

Verification: `tests/test_dar.py` builds synthetic archives (stored-zero,
stored and zlib entries) for list/extract/repack, byte-identical untouched
round-trips, both rewrite paths, traversal rejection and the named failure
codes. The user's `INI.DAR` is copied to a temporary directory, extracted and
repacked with no edits; the hash must be unchanged. The real-archive case
skips with `dar_fixture_missing` when the private ISO extraction is absent.

## Relationship to T8

T8 reads TPLs from the user's DARs only after extraction to a temporary or
user-chosen directory; it does not modify DARs. `tools/dar.py` (list/extract
and in-place rewrite) is T12 work. Texture dump/replacement and font-atlas
repacking operate on extracted files, so a DAR repack is a separate,
size-preserving step.

## References

- `kreycai/gungnir-ptbr` `tools/listar_dar.py` and `repack_gungnir.py`:
  header/entry layout, `stored_size == 0` handling, in-place rewrite and
  zero-fill discipline (format facts only; no translated content or assets).
- T3 intake knowledge in `.dex/plan.md` §2 and gotchas §7B.1–B.2.
- `third_party/yakumo` `iso_image.cpp` (MIT): ISO 9660 directory-record
  access for the both-endian size rule.
