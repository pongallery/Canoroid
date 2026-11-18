#!/usr/bin/env python3
"""Run with one real Canon JPEG. Everything written is under a temporary directory."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

tools = Path(__file__).resolve().parents[1]
camera = tools.parents[1] / "src" / "canoroid"
fixture = Path(sys.argv[1]).read_bytes()
subprocess.run(["make", "-C", tools], check=True)
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for header in ("dryos.h", "bmp.h", "fio-ml.h", "gui.h", "gui-common.h",
                   "lens.h", "task_utils.h", "zebra.h", "fileprefix.h", "vram.h"):
        (root / header).touch()
    binary = root / "session"
    build = tools / "build"
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-no-pie", "-I", root, "-I", build / "jpeg-9f",
                    tools / "tests/card_session.c", camera / "print.c", *sorted(build.glob("*.o")),
                    "-o", binary], check=True)
    photos = root / "ML/CANOROID"
    dcim = root / "B:/DCIM/100CANON"
    photos.mkdir(parents=True)
    dcim.mkdir(parents=True)
    (root / "staged.JPG").write_bytes(fixture)
    (root / "second.JPG").write_bytes(fixture + bytes(64))
    (dcim / "IMG_0100.JPG").write_bytes(fixture)
    (dcim / "IMG_0199.JPG").write_bytes(b"damaged JPEG")
    legacy = photos / "P0001.JPG"
    subprocess.run([build / "canoroid-print", root / "staged.JPG", legacy, "1"], check=True)
    subprocess.run([binary], cwd=root, check=True)
    originals = {f: hashlib.sha256(f.read_bytes()).digest() for f in [legacy, *dcim.glob("*.JPG"), *photos.glob("100*.JPG")]}
    (root / "staged.JPG").write_bytes(fixture)
    subprocess.run([binary, "reboot"], cwd=root, check=True)
    assert all(hashlib.sha256(f.read_bytes()).digest() == digest for f, digest in originals.items())
    assert (dcim / "IMG_0103.JPG").read_bytes() == fixture
    assert (photos / "100_0101.JPG").read_bytes() != (photos / "100_0102.JPG").read_bytes()
    assert "IMG_0199.JPG" in (photos / "E0001.TXT").read_text()
    assert "Input JPEG" in (root / "ML/CANOROID.LOG").read_text()
