#!/usr/bin/env python3
"""Third-party dependency ledger verifier — T2 (plan TC-2.1).

Reads dependencies.json at the given root and verifies every entry:

  type "vendored": the directory must exist and its content digest must
                   equal the pinned sha256 (canonical form: sorted relative
                   POSIX paths, each `path \\0 + sha256(file bytes)`).
  any other type:  DEPS_FETCH_UNSUPPORTED — no fetch-based dependency is in
                   use yet (Devs.cmake fetch support lands with the first
                   one; a silent skip would be a hidden failure, so this
                   names the gap instead).

Failures are named and loud: DEPS_MISSING / DEPS_DIGEST_MISMATCH /
DEPS_FETCH_UNSUPPORTED. Exit 0 only when every entry verifies.

Run: python3 tools/deps.py verify [--root REPO]
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path


class DepsError(Exception):
    def __init__(self, name, detail):
        super().__init__(f"{name}: {detail}")
        self.name = name
        self.detail = detail


def die(name, detail):
    raise DepsError(name, detail)


def digest_of_tree(tree: Path):
    if not tree.is_dir():
        die("DEPS_MISSING", f"vendored tree absent: {tree}")
    h = hashlib.sha256()
    files = sorted(p for p in tree.rglob("*") if p.is_file())
    if not files:
        die("DEPS_MISSING", f"vendored tree has no files: {tree}")
    for p in files:
        rel = p.relative_to(tree).as_posix()
        h.update(rel.encode())
        h.update(b"\0")
        h.update(hashlib.sha256(p.read_bytes()).digest())
    return h.hexdigest()


def load_dependencies(root: Path):
    path = root / "dependencies.json"
    if not path.is_file():
        die("DEPS_MANIFEST_MISSING", str(path))
    data = json.loads(path.read_text())
    deps = data.get("dependencies", [])
    if not deps:
        die("DEPS_MANIFEST_EMPTY", str(path))
    return deps


def deps(root: Path):
    for entry in load_dependencies(root):
        kind = entry.get("type", "vendored")
        tree = root / entry["path"]
        if kind != "vendored":
            die("DEPS_FETCH_UNSUPPORTED",
                f"dependency {entry['name']!r} declares type {kind!r}; "
                "fetch-based materialization (cmake/Deps.cmake) lands with "
                "the first fetch-type dependency — nothing to skip silently")
        actual = digest_of_tree(tree)
        pinned = entry.get("digest")
        if not pinned:
            die("DEPS_DIGEST_MISSING",
                f"dependency {entry['name']!r} has no pinned digest")
        if actual != pinned:
            die("DEPS_DIGEST_MISMATCH",
                f"dependency {entry['name']!r}: computed {actual} != pinned {pinned}")
        print(f"OK {entry['name']} @ {entry['commit'][:12]} ({entry['license']})")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("cmd", choices=["verify"])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    try:
        deps(root)
    except DepsError as e:
        print(f"DEPS_ERR {e.name}: {e.detail}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
