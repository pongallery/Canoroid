#!/usr/bin/env python3
"""Run with one real Canon JPEG. Everything written is under a temporary directory."""
import hashlib
from pathlib import Path
import struct
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
                   "lens.h", "propvalues.h", "lvinfo.h", "task_utils.h", "zebra.h", "fileprefix.h", "vram.h"):
        (root / header).touch()
    binary = root / "session"
    build = tools / "build"
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-no-pie", "-I", root, "-I", build / "jpeg-9f",
                    '-DCANOROID_VERSION="' + (tools.parents[1] / "VERSION").read_text().strip() + '"',
                    tools / "tests/card_session.c", camera / "print.c", *sorted(build.glob("*.o")),
                    "-o", binary], check=True)
    from zipfile import ZipFile
    sys.path.insert(0, str(tools))
    from tone_tables import tone_table
    tones = root / "ML/TONES"
    tones.mkdir(parents=True)
    with ZipFile(tools / "tones/sources.zip") as cubes:
        for name in ("FADED", "VIVID", "REDSCALE"):
            (tones / (name + ".LUT")).write_bytes(tone_table(cubes, name))
    old = root / "ML/CANOROID"
    old.mkdir()
    photos = root / "DCIM/CANOROID"
    dcim = root / "B:/DCIM/100CANON"

    dcim.mkdir(parents=True)
    (root / "DCIM").symlink_to(dcim.parent, target_is_directory=True)
    # Big-endian camera EXIF exercises conversion to the little-endian
    # DateTimeOriginal in a framed JPEG, including after gallery reopen.
    tiff = (b"MM" + struct.pack(">HI", 42, 8)
            + struct.pack(">HHHIII", 1, 0x8769, 4, 1, 26, 0)
            + struct.pack(">HHHIII", 1, 0x9003, 2, 20, 44, 0)
            + b"2024:02:29 19:45:16\x00")
    marker = b"\xff\xe1" + struct.pack(">H", 8 + len(tiff)) + b"Exif\x00\x00" + tiff
    (root / "staged.JPG").write_bytes(fixture[:2] + marker + fixture[2:])
    (root / "second.JPG").write_bytes(fixture + bytes(64))
    (dcim / "IMG_0100.JPG").write_bytes(fixture)
    (dcim / "IMG_0199.JPG").write_bytes(b"damaged JPEG")
    legacy = old / "P0001.JPG"
    subprocess.run([build / "canoroid-print", root / "staged.JPG", legacy, "1"], check=True)
    reference = root / "redscale.jpg"
    subprocess.run([build / "canoroid-print", root / "staged.JPG", reference, "1", "3"], cwd=root, check=True)
    subprocess.run([binary], cwd=root, check=True)
    assert (photos / "100_0101.JPG").read_bytes() == reference.read_bytes()
    assert reference.read_bytes() != legacy.read_bytes()
    assert (photos / "100_0101.JPG").read_bytes() != (photos / "100_0102.JPG").read_bytes()
    (dcim / "IMG_0102.CR2").write_bytes(b"RAW original sentinel")
    originals = {f: hashlib.sha256(f.read_bytes()).digest() for f in dcim.iterdir()}
    (root / "staged.JPG").write_bytes(fixture)
    for name in ("IMG_2144.JPG", "IMG_2147.JPG"):
        (dcim / name).write_bytes(fixture)
    (old / "EU.TXT").write_bytes(b"existing log")
    subprocess.run([binary, "reboot"], cwd=root, check=True)
    assert all(hashlib.sha256(f.read_bytes()).digest() == digest for f, digest in originals.items())
    assert (dcim / "IMG_0103.JPG").read_bytes() == fixture
    assert not list(photos.glob("*.JPG"))
    assert (old / "EU.TXT").read_bytes() == b"existing log"
    assert "100_2144.TMP" in (old / "E0001.TXT").read_text()
    assert "IMG_0199.JPG" in (old / "E0002.TXT").read_text()
    assert all((dcim / name).read_bytes() == fixture for name in ("IMG_2144.JPG", "IMG_2147.JPG"))
    assert "Input JPEG" in (root / "ML/CANOROID.LOG").read_text()
    other = dcim.parent / "101CANON"
    other.mkdir()
    (other / "IMG_0100.JPG").write_bytes(fixture)
    subprocess.run([binary, "batch"], cwd=root, check=True)
    batch_ref = root / "vivid-full.jpg"
    subprocess.run([build / "canoroid-print", dcim / "IMG_0100.JPG", batch_ref, "10", "2"], cwd=root, check=True)
    assert all((photos / name).read_bytes()==batch_ref.read_bytes() for name in ("100_0100.JPG","101_0100.JPG"))
    assert all(hashlib.sha256(f.read_bytes()).digest()==digest for f,digest in originals.items())
    assert (other / "IMG_0100.JPG").read_bytes()==fixture
    assert "IMG_0199.JPG" in (old / "E0003.TXT").read_text()
    print("Multi-select across folders, verified full-size tone, per-image failure and gallery passed")
