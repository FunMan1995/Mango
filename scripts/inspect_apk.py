#!/usr/bin/env python3
"""Inspect an APK's native ABIs the same way core/ CompatibilityChecker does."""

from __future__ import annotations

import argparse
import sys
import zipfile
from pathlib import Path

GUEST_32 = {"armeabi", "armeabi-v7a"}
OK_WITH_GUEST = GUEST_32 | {"x86"}


def inspect(apk: Path) -> tuple[set[str], bool]:
    abis: set[str] = set()
    saw_lib = False
    with zipfile.ZipFile(apk) as z:
        for name in z.namelist():
            parts = name.split("/")
            if len(parts) == 3 and parts[0] == "lib" and parts[2]:
                saw_lib = True
                abis.add(parts[1])
    return abis, saw_lib


def verdict(abis: set[str], saw_lib: bool, device_abis: set[str], bridge: bool) -> str:
    if not saw_lib:
        return "RUNS_NATIVELY"
    if abis & device_abis:
        return "RUNS_NATIVELY"
    if abis and abis <= OK_WITH_GUEST and (abis & GUEST_32):
        return "NEEDS_BRIDGE" if bridge else "BLOCKED"
    return "BLOCKED"


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("apk", type=Path, nargs="+")
    p.add_argument("--device-abi", action="append", default=["arm64-v8a"])
    p.add_argument("--bridge", action="store_true", default=True)
    args = p.parse_args()
    device = set(args.device_abi)
    rc = 0
    for apk in args.apk:
        abis, saw = inspect(apk)
        v = verdict(abis, saw, device, args.bridge)
        print(f"{apk.name}: abis={sorted(abis) or ['(none)']} native={saw} verdict={v}")
        if v == "BLOCKED":
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
