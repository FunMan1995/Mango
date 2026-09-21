# SPDX-FileCopyrightText: 2026 Drena Borg
# SPDX-License-Identifier: GPL-3.0-only
"""Inject Mango's Native Bridge .so into system.img for a signed OTA.

Not a Magisk module. Properties are written into build.prop so ART loads
libmango_translator.so without resetprop. Zygote stays 64-bit.
"""

from __future__ import annotations

import argparse
from collections.abc import Iterable
import logging
from pathlib import Path, PurePosixPath
import shutil
from typing import override

from lib.filesystem import CpioFs, ExtFs
from lib.modules import MissingArgs, Module, ModuleRequirements


logger = logging.getLogger(__name__)

SO_NAME = 'libmango_translator.so'
SO_PATH = PurePosixPath('system/lib64') / SO_NAME

PROP_FILES = (
    'system/build.prop',
    'system/etc/prop.default',
    'system/product/etc/build.prop',
    'system/system_ext/etc/build.prop',
)

PROPS = {
    'ro.dalvik.vm.native.bridge': SO_NAME,
    'ro.enable.native.bridge.exec': '1',
}


class MangoModule(Module):
    NAME = 'mango'

    @classmethod
    @override
    def add_args(cls, parser: argparse.ArgumentParser):
        parser.add_argument(
            '--module-mango-so',
            type=Path,
            help='path to libmango_translator.so (arm64-v8a)',
        )

    def __init__(self, args: argparse.Namespace) -> None:
        so: Path | None = args.module_mango_so
        if so is None:
            raise MissingArgs()
        if not so.is_file():
            raise FileNotFoundError(so)
        self.so = so

    @override
    def requirements(self) -> ModuleRequirements:
        return ModuleRequirements(
            boot_images=set(),
            ext_images={'system'},
            selinux_patching=False,
        )

    @override
    def inject(
        self,
        boot_fs: dict[str, CpioFs],
        ext_fs: dict[str, ExtFs],
        sepolicies: Iterable[Path],
    ) -> None:
        logger.info('Injecting Mango native bridge: %s', self.so)
        system_fs = ext_fs['system']

        system_fs.mkdir(SO_PATH.parent, mode=0o755, parents=True, exist_ok=True)
        with system_fs.open(SO_PATH, 'wb', mode=0o644) as f_out:
            with open(self.so, 'rb') as f_in:
                shutil.copyfileobj(f_in, f_out)

        for rel in PROP_FILES:
            self._patch_prop_file(system_fs, rel)

        self._ensure_abilist(system_fs)

    def _patch_prop_file(self, system_fs: ExtFs, rel: str) -> None:
        path = PurePosixPath(rel)
        try:
            with system_fs.open(path, 'r') as f:
                lines = f.read().splitlines()
        except FileNotFoundError:
            return

        keys = set(PROPS)
        out: list[str] = []
        seen: set[str] = set()
        for line in lines:
            stripped = line.strip()
            if stripped and not stripped.startswith('#') and '=' in stripped:
                key = stripped.split('=', 1)[0]
                if key in keys:
                    out.append(f'{key}={PROPS[key]}')
                    seen.add(key)
                    continue
            out.append(line)
        for key, value in PROPS.items():
            if key not in seen:
                out.append(f'{key}={value}')

        with system_fs.open(path, 'w') as f:
            f.write('\n'.join(out) + '\n')
        logger.info('Patched native-bridge props in %s', rel)

    def _ensure_abilist(self, system_fs: ExtFs) -> None:
        """Advertise ARM32 ABIs so PackageManager will install 32-bit APKs."""
        for rel in PROP_FILES:
            path = PurePosixPath(rel)
            try:
                with system_fs.open(path, 'r') as f:
                    text = f.read()
            except FileNotFoundError:
                continue
            if 'ro.product.cpu.abilist=' not in text and 'ro.system.product.cpu.abilist=' not in text:
                continue
            lines = text.splitlines()
            out: list[str] = []
            for line in lines:
                if line.startswith('ro.product.cpu.abilist=') or line.startswith(
                    'ro.system.product.cpu.abilist='
                ):
                    key, _, val = line.partition('=')
                    parts = [p for p in val.split(',') if p]
                    for extra in ('armeabi-v7a', 'armeabi'):
                        if extra not in parts:
                            parts.append(extra)
                    out.append(f'{key}={",".join(parts)}')
                elif line.startswith('ro.product.cpu.abilist32=') or line.startswith(
                    'ro.system.product.cpu.abilist32='
                ):
                    key, _, val = line.partition('=')
                    parts = [p for p in val.split(',') if p]
                    for extra in ('armeabi-v7a', 'armeabi'):
                        if extra not in parts:
                            parts.append(extra)
                    out.append(f'{key}={",".join(parts) if parts else "armeabi-v7a,armeabi"}')
                else:
                    out.append(line)
            with system_fs.open(path, 'w') as f:
                f.write('\n'.join(out) + '\n')
            logger.info('Patched cpu.abilist in %s', rel)
