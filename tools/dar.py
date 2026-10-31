#!/usr/bin/env python3
"""DAR archive reader and size-preserving in-place rewriter.

Format facts are documented in docs/formats/DAR.md. The tool never rebuilds an
archive: only entry payload slots are patched, so every data offset and every
internal absolute reference stays valid. Extract first, edit the extracted
files, then repack with --from.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path

HEADER = struct.Struct("<IIII")
ENTRY = struct.Struct("<IIII")
SECTOR = 0x200


class DarError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


@dataclass(frozen=True)
class Entry:
    index: int
    name: str
    name_offset: int
    stored_size: int
    decompressed_size: int
    data_offset: int

    @property
    def kind(self) -> str:
        if self.stored_size == 0:
            return "stored-zero"
        if self.decompressed_size > self.stored_size:
            return "zlib"
        return "stored"

    @property
    def payload_size(self) -> int:
        return self.decompressed_size if self.stored_size == 0 else self.stored_size


def _safe_name(name: str) -> bool:
    if not name or name.startswith("/") or "\\" in name or "\0" in name:
        return False
    return all(part not in ("", ".", "..") for part in name.split("/"))


class Archive:
    def __init__(self, path: Path, data: bytes):
        self.path = path
        self.data = data
        self.entries = self._parse()

    @classmethod
    def open(cls, path: Path) -> "Archive":
        try:
            return cls(path, path.read_bytes())
        except OSError as error:
            raise DarError("DAR_READ_FAILED", f"{path}: {error}") from error

    def _parse(self) -> list[Entry]:
        size = len(self.data)
        if size < HEADER.size:
            raise DarError("DAR_HEADER_TRUNCATED", f"{self.path} has no complete header")
        count, data_start, names_offset, table_offset = HEADER.unpack_from(self.data, 0)
        if count > 1_000_000:
            raise DarError("DAR_ENTRY_COUNT_INVALID", f"{count} entries is not credible")
        if table_offset < HEADER.size or table_offset + count * ENTRY.size > size:
            raise DarError("DAR_TABLE_OUT_OF_RANGE", f"table {table_offset:#x} count {count}")
        if names_offset >= size or data_start > size:
            raise DarError("DAR_SECTION_OUT_OF_RANGE", f"names {names_offset:#x} data {data_start:#x}")

        entries: list[Entry] = []
        seen: set[str] = set()
        for index in range(count):
            name_offset, stored_size, decompressed_size, data_offset = ENTRY.unpack_from(
                self.data, table_offset + index * ENTRY.size)
            name = self._name(name_offset)
            if name in seen:
                raise DarError("DAR_DUPLICATE_NAME", name)
            seen.add(name)
            if not _safe_name(name):
                raise DarError("DAR_INVALID_NAME", repr(name))
            if data_offset > size:
                raise DarError("DAR_DATA_OUT_OF_RANGE", f"{name}: offset {data_offset:#x}")
            length = decompressed_size if stored_size == 0 else stored_size
            if data_offset + length > size:
                raise DarError("DAR_DATA_OUT_OF_RANGE", f"{name}: {data_offset:#x}+{length:#x}")
            entries.append(Entry(index, name, name_offset, stored_size, decompressed_size, data_offset))
        return entries

    def _name(self, offset: int) -> str:
        if offset >= len(self.data):
            raise DarError("DAR_NAME_OUT_OF_RANGE", f"offset {offset:#x}")
        end = self.data.find(b"\0", offset)
        if end < 0:
            raise DarError("DAR_NAME_UNTERMINATED", f"offset {offset:#x}")
        try:
            return self.data[offset:end].decode("ascii")
        except UnicodeDecodeError as error:
            raise DarError("DAR_NAME_INVALID", f"offset {offset:#x}") from error

    def _decompress(self, name: str, data_offset: int, stored: int, size: int) -> bytes:
        raw = self.data[data_offset:data_offset + stored]
        try:
            value = zlib.decompress(raw)
        except zlib.error as error:
            raise DarError("DAR_INFLATE_FAILED", f"{name}: {error}") from error
        if len(value) != size:
            raise DarError("DAR_SIZE_MISMATCH", f"{name}: got {len(value)} expected {size}")
        return value

    def payload(self, entry: Entry) -> bytes:
        if entry.stored_size == 0:
            return self.data[entry.data_offset:entry.data_offset + entry.decompressed_size]
        if entry.decompressed_size > entry.stored_size:
            return self._decompress(entry.name, entry.data_offset,
                                    entry.stored_size, entry.decompressed_size)
        return self.data[entry.data_offset:entry.data_offset + entry.stored_size]

    def table_offset(self, index: int) -> int:
        return HEADER.unpack_from(self.data, 0)[3] + index * ENTRY.size

    def extract(self, out_dir: Path, force: bool = False) -> list[Entry]:
        out_dir.mkdir(parents=True, exist_ok=True)
        for entry in self.entries:
            target = out_dir / entry.name
            if target.exists() and not force:
                raise DarError("DAR_EXTRACT_EXISTS", f"{target} exists (use --force)")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(self.payload(entry))
        return self.entries


def compress_to_fit(data: bytes, capacity: int) -> bytes:
    if capacity <= 0:
        raise DarError("DAR_ENTRY_TOO_LARGE", "no slot capacity")
    best: bytes | None = None
    for level in range(9, 0, -1):
        blob = zlib.compress(data, level)
        if len(blob) <= capacity and (best is None or len(blob) < len(best)):
            best = blob
    if best is None:
        raise DarError("DAR_ENTRY_TOO_LARGE", f"{len(data)} bytes do not fit {capacity} bytes")
    return best


def repack(archive: Archive, source_dir: Path) -> tuple[bytearray, int, int]:
    """Patch payload slots in place. Returns (bytes, changed, unchanged)."""
    output = bytearray(archive.data)
    changed = 0
    unchanged = 0
    for entry in archive.entries:
        path = source_dir / entry.name
        try:
            replacement = path.read_bytes()
        except OSError as error:
            raise DarError("DAR_SOURCE_MISSING", f"{entry.name}: {error}") from error
        original = archive.payload(entry)
        if replacement == original:
            unchanged += 1
            continue
        if entry.stored_size == 0:
            if len(replacement) != entry.decompressed_size:
                raise DarError("DAR_STORED_SIZE_CHANGED",
                               f"{entry.name}: {len(replacement)} != {entry.decompressed_size}")
            output[entry.data_offset:entry.data_offset + entry.decompressed_size] = replacement
        elif entry.decompressed_size > entry.stored_size:
            if len(replacement) <= entry.stored_size:
                raise DarError("DAR_COMPRESSED_SLOT_TOO_SMALL",
                               f"{entry.name}: {len(replacement)} bytes would read back as stored data")
            blob = compress_to_fit(replacement, entry.stored_size)
            output[entry.data_offset:entry.data_offset + entry.stored_size] = (
                blob + b"\0" * (entry.stored_size - len(blob)))
            struct.pack_into("<I", output, archive.table_offset(entry.index) + 8, len(replacement))
        else:
            if len(replacement) != entry.stored_size:
                raise DarError("DAR_STORED_SIZE_CHANGED",
                               f"{entry.name}: {len(replacement)} != {entry.stored_size}")
            output[entry.data_offset:entry.data_offset + entry.stored_size] = replacement
            struct.pack_into("<I", output, archive.table_offset(entry.index) + 8, len(replacement))
        changed += 1
    return output, changed, unchanged


def write_in_place(path: Path, data: bytearray) -> None:
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def command_list(args) -> int:
    archive = Archive.open(Path(args.archive))
    for entry in archive.entries:
        print(f"{entry.name}\t{entry.decompressed_size}\t{entry.stored_size}\t{entry.kind}")
    print(f"# {len(archive.entries)} entries", file=sys.stderr)
    return 0


def command_extract(args) -> int:
    archive = Archive.open(Path(args.archive))
    archive.extract(Path(args.out), args.force)
    print(f"# extracted {len(archive.entries)} entries", file=sys.stderr)
    return 0


def command_repack(args) -> int:
    archive = Archive.open(Path(args.archive))
    output, changed, unchanged = repack(archive, Path(args.source))
    before = hashlib.sha256(archive.data).hexdigest()
    write_in_place(archive.path, output)
    after = hashlib.sha256(archive.path.read_bytes()).hexdigest()
    print(f"# changed {changed} unchanged {unchanged} sha256 {before} -> {after}", file=sys.stderr)
    return 0


def command_info(args) -> int:
    archive = Archive.open(Path(args.archive))
    count, data_start, names_offset, table_offset = HEADER.unpack_from(archive.data, 0)
    print(f"entries={count} data_start={data_start} names={names_offset} table={table_offset} "
          f"size={len(archive.data)}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    listing = sub.add_parser("list", help="list entries")
    listing.add_argument("archive")
    listing.set_defaults(func=command_list)

    info = sub.add_parser("info", help="print header fields")
    info.add_argument("archive")
    info.set_defaults(func=command_info)

    extract = sub.add_parser("extract", help="extract every entry")
    extract.add_argument("archive")
    extract.add_argument("--out", required=True)
    extract.add_argument("--force", action="store_true")
    extract.set_defaults(func=command_extract)

    repack_parser = sub.add_parser("repack", help="patch payload slots from extracted files")
    repack_parser.add_argument("archive")
    repack_parser.add_argument("--from", dest="source", required=True)
    repack_parser.set_defaults(func=command_repack)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except DarError as error:
        print(f"dar: {error.code}: {error.message}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
