"""Validate Safari extension icons before archive and in the exported IPA."""
import json
from pathlib import Path, PurePosixPath
import struct
import sys


def verify(read):
    manifest = json.loads(read("manifest.json"))
    if not isinstance(manifest, dict):
        raise ValueError("Safari manifest must be an object")
    icons = manifest.get("icons")
    if not isinstance(icons, dict) or not icons:
        raise ValueError("Safari manifest requires top-level icons (App Store error 90849)")
    for size, relative in icons.items():
        if not size.isascii() or not size.isdecimal() or int(size) <= 0 or not isinstance(relative, str):
            raise ValueError("Safari icons must map numeric sizes to resource paths")
        path = PurePosixPath(relative)
        if path.is_absolute() or ".." in path.parts or "\\" in relative or path.suffix.lower() != ".png":
            raise ValueError("Safari icon must be a relative PNG resource")
        image = read(relative)
        if len(image) < 24 or image[:16] != b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR":
            raise ValueError("Safari icon is not a PNG: " + relative)
        if struct.unpack(">II", image[16:24]) != (int(size), int(size)):
            raise ValueError("Safari icon dimensions differ from manifest: " + relative)
    return True


if __name__ == "__main__":
    try:
        root = Path(sys.argv[1])
        verify(lambda name: (root / name).read_bytes())
        print("Safari manifest icons verified")
    except (ValueError, KeyError, OSError) as error:
        sys.exit(str(error))
