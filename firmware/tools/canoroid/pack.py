#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import sys
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

root, build = map(Path, sys.argv[1:])
version = (root / "VERSION").read_text().strip()
payload = {
    "autoexec.bin": build / "autoexec.bin",
    "COPYING": root / "COPYING",
    "CANOROID.md": root / "CANOROID.md",
    "ML/CANOROID-IJG.txt": build / "jpeg-9f" / "README",
}
fonts = root / "data" / "fonts"
for font in sorted(fonts.glob("*.rbf")):
    payload[f"ML/fonts/{font.name}"] = font

for package in ("FIRST-INSTALL", "UPDATE"):
    entries = dict(payload)
    if package == "FIRST-INSTALL":
        entries["ML-SETUP.FIR"] = root / "platform" / "600D.102" / "ML-SETUP.FIR"
    name = build / f"Canoroid-v{version}-{package}.zip"
    temporary = name.with_suffix(".tmp")
    with ZipFile(temporary, "w", ZIP_DEFLATED) as archive:
        for target, source in entries.items():
            archive.write(source, target)
    temporary.replace(name)
    print(name)
