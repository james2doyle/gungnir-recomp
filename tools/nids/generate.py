#!/usr/bin/env python3
"""Generate configs/nids.csv from pinned PSPSDK import stubs (T6a).

PSPSDK is BSD-3-Clause; its `IMPORT_FUNC "Library",0xNID,name` lines are the
authoritative user-mode import inventory. The generated CSV is committed so a
build never needs network access; this script exists to refresh it against a
pinned upstream commit and to record the provenance.

Usage:
  python3 tools/nids/generate.py --ref <pspsdk-commit> \
      --out configs/nids.csv --pin configs/nids.csv.sha256
"""
import argparse
import hashlib
import re
import sys
import urllib.request
from pathlib import Path

# Library -> PSPSDK import-stub sources. Multiple files are allowed where a
# library's imports are split across a driver/base file.
SOURCES = {
    "ThreadManForUser": ["src/user/ThreadManForUser.S"],
    "sceSasCore": ["src/sascore/sceSasCore.S"],
    "sceMpeg": ["src/mpeg/sceMpeg.S", "src/mpeg/sceMpegbase.S"],
    "IoFileMgrForUser": ["src/user/IoFileMgrForUser.S"],
    "sceUtility": ["src/utility/sceUtility.S"],
    "sceGe_user": ["src/ge/sceGe_user.S"],
    "sceAudio": ["src/audio/sceAudio.S"],
    "sceDisplay": ["src/display/sceDisplay.S"],
    "sceAtrac3plus": ["src/atrac3/sceAtrac3plus.S"],
    "Kernel_Library": ["src/user/Kernel_Library.S"],
    "SysMemUserForUser": ["src/user/SysMemUserForUser.S"],
    "UtilsForUser": ["src/user/UtilsForUser.S"],
    "sceUmdUser": ["src/umd/sceUmdUser.S"],
    "ModuleMgrForUser": ["src/user/ModuleMgrForUser.S"],
    "sceCtrl": ["src/ctrl/sceCtrl.S"],
    "InterruptManager": ["src/user/InterruptManager.S"],
    "StdioForUser": ["src/user/StdioForUser.S"],
    "sceSuspendForUser": ["src/user/sceSuspendForUser.S"],
    "LoadExecForUser": ["src/user/LoadExecForUser.S"],
    "scePower": ["src/power/scePower.S"],
    "sceDmac": ["src/dmac/sceDmac.S"],
    "sceImpose": ["src/user/sceImpose.S"],
    "sceRtc": ["src/rtc/sceRtc.S"],
}

# NID/name pairs that PSPSDK master does not export but Gungnir's main module
# imports. Values and names are public interface facts cross-checked against
# PPSSPP/UOFW NID tables; they are recorded separately from the PSPSDK-derived
# rows so a refresh cannot silently drop them.
SUPPLEMENT = [
    ("sceDisplay", 0x77ED8B3A, "sceDisplayWaitVblankStartMultiCB"),
    ("Kernel_Library", 0x293B45B8, "sceKernelGetThreadId"),
    ("Kernel_Library", 0xA089ECA4, "sceKernelMemset"),
    ("Kernel_Library", 0x1839852A, "sceKernelMemcpy"),
    ("ModuleMgrForUser", 0xF0A26395, "sceKernelGetModuleId"),
    ("ModuleMgrForUser", 0x8F2DF740, "sceKernelStopUnloadSelfModuleWithStatus"),
    ("SysMemUserForUser", 0xF77D77CB, "sceKernelSetCompilerVersion"),
    ("SysMemUserForUser", 0x1B4217BC, "sceKernelSetCompiledSdkVersion603_605"),
    ("ThreadManForUser", 0xF8170FBE, "sceKernelDeleteMutex"),
    ("ThreadManForUser", 0x6B30100F, "sceKernelUnlockMutex"),
    ("ThreadManForUser", 0xB011B11F, "sceKernelLockMutex"),
    ("ThreadManForUser", 0xB7D098C6, "sceKernelCreateMutex"),
    ("sceMpeg", 0x4571CC64, "sceMpegAvcDecodeFlush"),
    ("scePower", 0x469989AD, "scePowerSetClockFrequency630"),
    ("sceUtility", 0x180F7B62, "sceUtilityGamedataInstallAbort"),
    ("sceUtility", 0x24AC31EB, "sceUtilityGamedataInstallInitStart"),
    ("sceUtility", 0x32E32DCB, "sceUtilityGamedataInstallShutdownStart"),
    ("sceUtility", 0x4AECD179, "sceUtilityGamedataInstallUpdate"),
    ("sceUtility", 0xB57E95D9, "sceUtilityGamedataInstallGetStatus"),
]

IMPORT_RE = re.compile(
    r'IMPORT_FUNC\s+"([^"]+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*([A-Za-z_][A-Za-z0-9_$]*)')
RAW = "https://raw.githubusercontent.com/pspdev/pspsdk/{ref}/{path}"


def fetch(ref, path):
    with urllib.request.urlopen(RAW.format(ref=ref, path=path), timeout=60) as response:
        return response.read().decode("utf-8")


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--ref", required=True)
    parser.add_argument("--out", default="configs/nids.csv")
    parser.add_argument("--pin", default="configs/nids.csv.sha256")
    args = parser.parse_args(argv)

    symbols = {}
    for library, paths in SOURCES.items():
        for path in paths:
            text = fetch(args.ref, path)
            for line in text.splitlines():
                match = IMPORT_RE.search(line)
                if not match:
                    continue
                found_library, nid, name = match.groups()
                if found_library not in SOURCES:
                    continue
                key = (found_library, int(nid, 16))
                if key in symbols and symbols[key] != name:
                    raise SystemExit(
                        f"conflicting names for {found_library}:{nid}: "
                        f"{symbols[key]!r} vs {name!r}")
                symbols[key] = name
    for library, nid, name in SUPPLEMENT:
        symbols[(library, nid)] = name

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# library,nid,name"]
    for (library, nid), name in sorted(symbols.items(),
                                       key=lambda item: (item[0][0], item[0][1])):
        lines.append(f"{library},0x{nid:08X},{name}")
    payload = ("\n".join(lines) + "\n").encode("utf-8")
    out.write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    Path(args.pin).write_text(f"{digest}  {out.as_posix()}\n")
    print(f"wrote {len(symbols)} symbols -> {out}")
    print(f"sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
