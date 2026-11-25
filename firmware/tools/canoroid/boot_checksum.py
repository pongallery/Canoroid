"""Check the 600D loader's XOR over little-endian 32-bit words."""
import struct
import sys
from pathlib import Path


def checksum(image):
    if len(image) < 32 or len(image) % 32 or image[-8:-4] != bytes.fromhex("13 ff 2f e1"):
        raise ValueError("Invalid 600D boot image length or footer")
    value = 0
    for (word,) in struct.iter_unpack("<I", image):
        value ^= word
    return value


if __name__ == "__main__":
    path = Path(sys.argv[1])
    image = bytearray(path.read_bytes())
    if image[-4:] != b"\xcc" * 4:
        raise ValueError("Expected a freshly linked boot image")
    struct.pack_into("<I", image, len(image) - 4, checksum(image) ^ 0xCCCCCCCC)
    path.write_bytes(image)
    if checksum(path.read_bytes()):
        raise ValueError("600D boot checksum failed after writing")
