#!/usr/bin/env python3
"""Exercise native JPEG scaling, centered crop and row reuse (requires Pillow)."""
from pathlib import Path
import subprocess
import tempfile

from PIL import Image

tools = Path(__file__).resolve().parents[1]
subprocess.run(["make", "-C", tools], check=True)
binary = tools / "build/canoroid-print"
frames = [(112, 112, 780, 4, (255, 255, 255)),
          (72, 42, 936, 0, (241, 212, 170)),
          (54, 54, 972, 0, (240, 247, 239))]
colors = [(210, 60, 40), (35, 180, 65), (40, 60, 190), (220, 180, 30)]

def check_pixel(image, xy, expected, tolerance=10):
    actual = image.getpixel(xy)
    assert max(abs(a - b) for a, b in zip(actual, expected)) <= tolerance, (xy, actual, expected)

with tempfile.TemporaryDirectory() as directory:
    card = Path(directory)
    # All three IDCT ratios, odd dimensions, portrait/landscape and JPEG
    # chroma sampling. Off-center color boundaries expose a misplaced crop.
    for width, height, subsampling in [(781, 467, 0), (2161, 3243, 1), (6485, 4321, 2)]:
        short = min(width, height)
        left, top = (width - short) // 2, (height - short) // 2
        split_x, split_y = left + short // 3, top + 2 * short // 3
        image = Image.new("RGB", (width, height), colors[0])
        image.paste(colors[1], (split_x, 0, width, split_y))
        image.paste(colors[2], (0, split_y, split_x, height))
        image.paste(colors[3], (split_x, split_y, width, height))
        source = card / "source.jpg"
        image.save(source, quality=94, subsampling=subsampling)
        for n, (inset, y0, h, outline, paper) in enumerate(frames, 1):
            out = card / f"{width}-{n}.jpg"
            subprocess.run([binary, source, out, str(n)], check=True)
            with Image.open(out) as result:
                assert result.size == (1080, 1080)
                for xy in [(12, 12), (12, 550), (540, y0 - outline - 10),
                           (540, y0 + h + outline + 10), (1062, 1062)]:
                    check_pixel(result, xy, paper)
                for x in (350, 370):
                    for y in (710, 730):
                        check_pixel(result, (x, y), colors[(x > 360) + 2 * (y > 720)])
                if outline:
                    for xy in [(540, y0 - 2), (540, y0 + h + 1), (inset - 2, 500)]:
                        check_pixel(result, xy, (0, 0, 0), tolerance=12)
    source.write_bytes(source.read_bytes()[:-512])
    out = card / "truncated.jpg"
    bad = subprocess.run([binary, source, out, "1"], capture_output=True)
    assert bad.returncode and b"Input JPEG" in bad.stderr
    assert not out.exists() and not out.with_suffix(".jpg.tmp").exists()
print("All decode scales, crop boundaries, frame edges and truncated-input checks passed")
