# DAR — Gungnir asset archives

Status: format facts for T8/T12. DAR extraction/repack tooling is T12; no DAR
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
      names: NUL-terminated ASCII, addressed by name_off
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

Payloads use zlib. `stored_size == decompressed_size` is stored data;
`decompressed_size > stored_size` is a zlib stream. A payload that fails to
inflate, a `data_off` outside the archive, or a name offset without a NUL
terminator is an error with a named code, never a silent skip.

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
