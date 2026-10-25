#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Split the verified signed Cityman ADSP ELF without modifying its contents.

Download qcadsp8994.mbn from Android4Lumia950/firmware-microsoft-cityman at
f489e275f1d32656c00a289ba57917735edff3bd. Firmware is not included here.
"""
import argparse
import hashlib
from pathlib import Path
import struct


SOURCE_SHA256 = "3e101699540296154c1ecaeb3effb7836649e47b72019e90faae341163ea835e"


def split_image(image):
    if hashlib.sha256(image).hexdigest() != SOURCE_SHA256:
        raise ValueError("input is not the verified Cityman qcadsp8994.mbn")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", image)
    if image[:7] != b"\x7fELF\x01\x01\x01" or header[5] != 52 or header[9] != 32:
        raise ValueError("expected ELF32 little-endian program headers")
    count = header[10]
    table_end = header[5] + count * header[9]
    if not count or table_end > len(image):
        raise ValueError("invalid program header table")
    segments = [struct.unpack_from("<IIIIIIII", image, header[5] + i * 32)
                for i in range(count)]
    first = segments[0]
    if first[0] != 0 or first[1] != 0 or first[4] < table_end:
        raise ValueError("missing complete ELF header segment")
    files = {}
    hashes = []
    for index, segment in enumerate(segments):
        offset, size, flags = segment[1], segment[4], segment[6]
        if offset > len(image) or size > len(image) - offset:
            raise ValueError("segment extends beyond input")
        payload = image[offset:offset + size]
        if size:
            files[f"adsp.b{index:02d}"] = payload
        if (flags >> 24) & 7 == 2:
            hashes.append(payload)
    if len(hashes) != 1 or not hashes[0]:
        raise ValueError("expected one nonempty signed hash segment")
    # PIL expects the original ELF headers followed by the signed hash segment.
    # Preserve every program-header index, payload, address, flag and signature.
    files["adsp.mdt"] = image[:first[4]] + hashes[0]
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("output", type=Path, help="new directory; must not exist")
    args = parser.parse_args()
    try:
        files = split_image(args.image.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"error: {error}\n")
    try:
        args.output.mkdir(parents=True, exist_ok=False)
        manifest = []
        for name, payload in sorted(files.items()):
            (args.output / name).write_bytes(payload)
            manifest.append(f"{hashlib.sha256(payload).hexdigest()}  {name}\n")
        (args.output / "sha256sums.txt").write_text("".join(manifest))
    except OSError as error:
        parser.exit(1, f"error: {error}\n")
    print(f"Wrote {len(files)} ADSP files and sha256sums.txt to {args.output}")


if __name__ == "__main__":
    main()
