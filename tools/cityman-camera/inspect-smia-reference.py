#!/usr/bin/env python3
"""Read the pinned public SMIA++ source and verify the first rear mode contract.

No ELF parsing, binary patching, I2C operations or phone access. The JSON is a
reference for a future sensor module, not a replacement for that module.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


REVISION = "6ab0e0e9018d6185b642fa86de8b1c61cbd2701b"
BASE = "drivers/media/platform/msm/camera_v2/sensor/"


def source(repo, filename):
    return subprocess.check_output(
        ["git", "-C", str(repo), "show", REVISION + ":" + BASE + filename]
    )


def program(text, name):
    match = re.search(
        r"static const struct smia65pp_regval " + re.escape(name)
        + r"\[\] = \{(.*?)^\};", text, re.M | re.S
    )
    if not match:
        raise ValueError("Missing register program: " + name)
    entries = [(int(a, 16), int(v, 16)) for a, v in re.findall(
        r"\{\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\s*\}", match[1]
    )]
    # The public mode arrays end with a zero sentinel, not a writable ID reg.
    if entries and entries[-1] == (0, 0):
        entries.pop()
    if not entries or any(a in (0, 0x0100, 0x0104) for a, _ in entries):
        raise ValueError("Unexpected identity/stream/group-hold register: " + name)
    return entries


def word(regs, address):
    return (regs[address] << 8) | regs[address + 1]


def inspect(repo):
    tables = source(repo, "smia65pp_modes.h")
    driver = source(repo, "smia65pp.c")
    mode = program(tables.decode(), "smia65pp_mode4")
    idle = program(tables.decode(), "smia65pp_idle")
    regs, analog = dict(mode), dict(idle)
    clock = int(re.search(rb"#define SMIA65PP_MCLK_HZ\s+(\d+)", driver)[1])
    geometry = re.search(
        rb"\{\s*4,\s*2496,\s*1872,\s*(\d+),\s*0x2b,\s*CSI_DECODE_10BIT\s*\}",
        driver,
    )
    if not geometry:
        raise ValueError("Missing matching RAW10 geometry in the reference driver")
    frame_lines = int(geometry[1])
    width, height = word(regs, 0x034c), word(regs, 0x034e)
    line_pixels = word(regs, 0x0342)
    pixel_clock = (clock * word(regs, 0x0306) * 4
                   // (word(regs, 0x0304) * word(regs, 0x0302) * word(regs, 0x0300)))
    expected = (9600000, 0x0999, 2496, 1872, 0x0a0a, 4, 6024, 2550, 460800000)
    actual = (clock, word(analog, 0x0136), width, height, word(regs, 0x0112),
              regs[0x0114] + 1, line_pixels, frame_lines, pixel_clock)
    if actual != expected:
        raise ValueError("Reference mode does not match the reviewed contract: " + repr(actual))
    return {
        "status": "reference only; no sensor-library ABI or hardware validation",
        "source": {
            "repository": "https://github.com/hraj9258/android_kernel_mmo_msm8994",
            "revision": REVISION,
            "tables_sha256": hashlib.sha256(tables).hexdigest(),
            "driver_sha256": hashlib.sha256(driver).hexdigest(),
        },
        "rear_mode": {
            "dcc_mode": 4, "mclk_hz": clock, "extclk_register": "0x0999",
            "width": width, "height": height, "lanes": 4,
            "format": "uncompressed RAW10", "csi_data_type": "0x2b",
            "decode": "CSI_DECODE_10BIT", "line_length_pck": line_pixels,
            "frame_length_lines": frame_lines, "vt_pixel_clock_hz": pixel_clock,
            "nominal_fps": round(pixel_clock / (line_pixels * frame_lines), 5),
            "mode_registers_without_sentinel": len(mode),
            "idle_registers_without_sentinel": len(idle),
        },
        "missing": ["matching angler sensor module and its build interface",
                    "complete sensor power/configure/start/stop integration",
                    "Cityman image-delivery validation", "front Ducati programming/module"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        result = inspect(args.repo)
    except (ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        parser.exit(1, "Cannot validate the pinned reference: " + str(error) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
