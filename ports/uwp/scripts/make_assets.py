#!/usr/bin/env python3
"""Validate the checked-in UWP artwork assets without overwriting them.

Artwork is preserved as project data so a routine build cannot silently restore
the old geometric placeholders over the user's logo or gameplay bezel.
"""
from pathlib import Path
import struct


EXPECTED = {
    "StoreLogo.png": (50, 50, False),
    "Square44x44Logo.png": (44, 44, False),
    "Square150x150Logo.png": (150, 150, False),
    "Wide310x150Logo.png": (310, 150, False),
    "SplashScreen.png": (620, 300, False),
    "GameplayOverlay.png": (1366, 768, True),
    "GameplayOverlay2.png": (1366, 768, True),
}


def main():
    assets = Path(__file__).resolve().parents[1] / "Assets"
    for name, (expected_width, expected_height, needs_alpha) in EXPECTED.items():
        path = assets / name
        if not path.is_file():
            raise SystemExit(f"Missing artwork asset: {path}")
        data = path.read_bytes()
        if len(data) < 26 or data[:8] != b"\x89PNG\r\n\x1a\n":
            raise SystemExit(f"Invalid PNG: {path}")
        width, height = struct.unpack(">II", data[16:24])
        color_type = data[25]
        if (width, height) != (expected_width, expected_height):
            raise SystemExit(f"Unexpected size for {name}: {width}x{height}")
        if needs_alpha and color_type not in (4, 6):
            raise SystemExit(f"{name} must include an alpha channel for its transparent viewport")
        print(f"OK {name}: {width}x{height}" + (" with alpha" if needs_alpha else ""))


if __name__ == "__main__":
    main()
