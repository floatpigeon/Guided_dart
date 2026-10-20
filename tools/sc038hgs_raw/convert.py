#!/usr/bin/env python3
"""Preview a capture under an explicitly selected RAW10 packing hypothesis."""
import argparse
import json
import math
from pathlib import Path
import struct


def unpack(data, width, height, stride, layout):
    if width <= 0 or height <= 0 or stride <= 0 or len(data) != height * stride:
        raise ValueError("Dimensions/stride do not match the captured byte count")
    row_bytes = width * 2 if layout.startswith("le16") else width // 4 * 5
    if (layout == "mipi10" and width % 4) or stride < row_bytes:
        raise ValueError("Invalid row size for the selected packing")
    pixels = []
    for y in range(height):
        row = data[y * stride:y * stride + row_bytes]
        if layout.startswith("le16"):
            words = struct.unpack("<" + "H" * width, row)
            mask = 0xfc00 if layout == "le16-lsb" else 0x003f
            if any(word & mask for word in words):
                raise ValueError("Nonzero unused bits: selected 16-bit alignment does not fit")
            pixels.extend(words if layout == "le16-lsb" else (word >> 6 for word in words))
        else:
            for x in range(0, row_bytes, 5):
                if layout == "packed10-lsb":
                    word = int.from_bytes(row[x:x + 5], "little")
                    pixels.extend((word >> (10 * i)) & 1023 for i in range(4))
                else:
                    pixels.extend((row[x + i] << 2) | ((row[x + 4] >> (2 * i)) & 3)
                                  for i in range(4))
    return pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--layout", required=True, choices=("le16-lsb", "le16-msb", "mipi10", "packed10-lsb"))
    parser.add_argument("--stride", type=int, help="Verified data row size; explicitly allows ignoring an oversized DMA tail")
    parser.add_argument("--gain", type=float, default=1.0, help="Preview gain only; default RAW10 >> 2")
    args = parser.parse_args()
    if not math.isfinite(args.gain) or args.gain <= 0:
        parser.error("gain must be finite and positive")
    metadata = json.loads(Path(str(args.raw) + ".json").read_text())
    width, height = metadata["width"], metadata["height"]
    stride = args.stride if args.stride is not None else metadata.get("data_stride", metadata["requested_stride"])
    data = args.raw.read_bytes()
    if args.stride is not None and stride > 0 and len(data) > height * stride:
        print(f"Explicit stride: ignoring {len(data) - height * stride} bytes beyond the verified frame")
        data = data[:height * stride]
    try:
        pixels = unpack(data, width, height, stride, args.layout)
    except ValueError as error:
        parser.error(str(error))
    preview = bytes(min(255, int(value * args.gain / 4)) for value in pixels)
    with args.output.open("xb") as out:
        out.write(f"P5\n{width} {height}\n255\n".encode() + preview)
    print(f"RAW10 hypothesis {args.layout}: min={min(pixels)} max={max(pixels)} "
          f"mean={sum(pixels)/len(pixels):.3f}; preview gain={args.gain:g}")
    print("Check the sensor gradient before accepting this packing; a preview alone is not proof.")


if __name__ == "__main__":
    main()
