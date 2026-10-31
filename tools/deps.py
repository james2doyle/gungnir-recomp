#!/usr/bin/env python3
"""Third-party dependency ledger verifier — T2 (plan TC-2.1).

Reads dependencies.json at the given root and verifies every entry:

  type "vendored": the directory must exist and its content digest must
                   equal the pinned sha256 (canonical form: sorted relative
                   POSIX paths, each `path \\0 + sha256(file bytes)`).
  type "fetch":    the entry must carry version/url/archive_sha256; the
                   archive is verified when present under build/android-deps,
                   otherwise the pin is reported as not materialized. The
                   Android build materializes it through cmake/Deps.cmake.
  any other type:  DEPS_FETCH_UNSUPPORTED — named gap, never a silent skip.

Failures are named and loud: DEPS_MISSING / DEPS_DIGEST_MISMATCH /
DEPS_PIN_MISSING / DEPS_FETCH_UNSUPPORTED. Exit 0 only when every entry
verifies or is a correctly pinned, unmaterialized fetch entry.

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
        if kind == "fetch":
            pinned = entry.get("archive_sha256")
            url = entry.get("url")
            version = entry.get("version")
            if not pinned or not url or not version:
                die("DEPS_PIN_MISSING",
                    f"dependency {entry['name']!r} needs version/url/archive_sha256")
            archive = root / "build" / "android-deps" / url.rsplit("/", 1)[-1]
            if archive.is_file():
                actual = hashlib.sha256(archive.read_bytes()).hexdigest()
                if actual != pinned:
                    die("DEPS_DIGEST_MISMATCH",
                        f"dependency {entry['name']!r}: archive {actual} != pinned {pinned}")
                print(f"OK {entry['name']} @ {version} ({entry['license']}) materialized")
            else:
                print(f"OK {entry['name']} @ {version} ({entry['license']}) not materialized")
            continue
        if kind != "vendored":
            die("DEPS_FETCH_UNSUPPORTED",
                f"dependency {entry['name']!r} declares type {kind!r}; "
                "only vendored and fetch entries are supported")
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
