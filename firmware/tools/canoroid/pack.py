#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import sys
from boot_checksum import checksum
from tone_tables import tone_table
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

root, build = map(Path, sys.argv[1:])
platform = build.resolve().parent
if platform.parent != (root / "platform").resolve() or platform.name not in {
    "550D.109", "600D.102", "6D.116", "650D.104"
}:
    raise ValueError(f"Unrecognized camera build directory: {build}")
port = platform.name != "600D.102"
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

# The stock 600D overlay enumerates this directory even when its cropmarks
# are disabled. Ship the same bitmaps as the Magic Lantern platform package.
for bitmap in sorted((root / "data" / "cropmks").glob("*.bmp")):
    payload[f"ML/CROPMKS/{bitmap.name}"] = bitmap

for package in ("FIRST-INSTALL", "UPDATE"):
    entries = dict(payload)
    if package == "FIRST-INSTALL":
        entries["ML-SETUP.FIR"] = platform / "ML-SETUP.FIR"
    target_suffix = f"-{platform.name}" if port else ""
    if port:
        entries["600D-REFERENCE.md"] = entries["CANOROID.md"]
        entries["CANOROID.md"] = root / "PORTS.md"
    name = build / f"Canoroid-v{version}{target_suffix}-{package}.zip"
    temporary = name.with_suffix(".tmp")
    with ZipFile(temporary, "w", ZIP_DEFLATED) as archive:
        archive.writestr("ML/CANOROID/", b"")
        tone_sources = root / "tools/canoroid/tones/sources.zip"
        archive.write(tone_sources, "ML/TONES/SOURCES.ZIP")
        with ZipFile(tone_sources) as sources:
            for tone_name in ("FADED", "VIVID", "REDSCALE"):
                table = tone_table(sources, tone_name)
                archive.writestr(f"ML/TONES/{tone_name}.LUT", table)
        for target, source in entries.items():
            archive.write(source, target)
    with ZipFile(temporary) as archive:
        if checksum(archive.read("autoexec.bin")):
            raise ValueError("Packaged autoexec.bin would fail the boot checksum")
    temporary.replace(name)
    print(name)
