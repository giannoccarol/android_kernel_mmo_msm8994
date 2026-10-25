#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Derive the cityman rear sensor library from the ROM's bullhead IMX377 one.

The camera HAL in the current LineageOS 18.1 ROM (camera.msm8992.so) only
reads the sensor library layout it was built with. The rear Lumia sensor is
an IMX230, so the bullhead libmmcamera_imx377.so is reused as a container and
these fields are rewritten:

  - sensor name "imx377" -> "imx230"
  - EEPROM and actuator names cleared (the Lumia parts are unknown); the
    flash stays "pmic": the daemon fails the session without a flash lib
  - slave 0x20, WORD address, ID register 0x0016, expected ID 0x0230
  - power-up order VAF, VANA, VDIG, VIO, reset, standby, MCLK 9.6 MHz,
    matching common/camera-cityman.dtsi

With this library the kernel prints
"msm_sensor_match_id: imx230 read id 0x230 expected 0x230 slave 0x20"
and "imx230 probe succeeded". Register tables and modes are still IMX377
ones: this is a probe library, not a streaming one.

No vendor binary is stored here. Pull the input from the phone:

  adb pull /vendor/lib/libmmcamera_imx377.so
  ./make-imx230-lib.py libmmcamera_imx377.so libmmcamera_imx230.so
"""
import hashlib
import sys

INPUT_SHA256 = "e8aa63374ea4cac20af853afb3a860f5bed9e989de4bb44f6c0cc654fac5bee1"
OUTPUT_SHA256 = "0bea68225612a1608abff47c2bd475a8126fab38007f9e48426d17cb46c9d27f"

# (offset, original bytes, new bytes), hex encoded
PATCH = [
    (0x300b, "333737", "323330"),
    (0x3028, "6d323463363473", "00000000000000"),
    (0x3048, "6c633839383231327864", "00000000000000000000"),
    (0x30ac, "34", "20"),
    (0x30b8, "44303200", "16003002"),
    (0x30c4, "00", "03"),
    (0x30c8, "00", "01"),
    (0x30cc, "00", "01"),
    (0x3100, "00", "01"),
    (0x3104, "00", "01"),
    (0x3134, "01", "00"),
    (0x3138, "00", "01"),
    (0x313c, "00", "01"),
    (0x3168, "01", "02"),
    (0x316c, "00", "01"),
    (0x3170, "00", "01"),
    (0x31a8, "02", "01"),
    (0x31ac, "00", "1e"),
    (0x31d8, "00", "01"),
    (0x31dc, "00", "01"),
    (0x31e0, "00366e0101", "010000001e"),
    (0x3219, "0000", "7c92"),
    (0x321c, "00", "05"),
    (0x3364, "06", "07"),
    (0x3374, "00", "01"),
    (0x33a4, "00", "01"),
    (0x33a8, "02", "00"),
    (0x33ac, "00", "01"),
    (0x33e4, "00", "01"),
    (0x341c, "00", "01"),
    (0x344c, "02", "00"),
    (0x3454, "00", "01"),
    (0x3484, "00", "02"),
    (0x348c, "00", "01"),
    (0x34b8, "00", "02"),
    (0x34bc, "00", "03"),
    (0x34c4, "00", "01"),
    (0x360c, "06", "07"),
]


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: make-imx230-lib.py <libmmcamera_imx377.so> <output>")
    data = bytearray(open(sys.argv[1], "rb").read())
    digest = hashlib.sha256(data).hexdigest()
    if digest != INPUT_SHA256:
        raise SystemExit("unexpected input sha256 %s" % digest)
    for offset, old, new in PATCH:
        old, new = bytes.fromhex(old), bytes.fromhex(new)
        if data[offset:offset + len(old)] != old:
            raise SystemExit("unexpected bytes at 0x%x" % offset)
        data[offset:offset + len(new)] = new
    digest = hashlib.sha256(data).hexdigest()
    if digest != OUTPUT_SHA256:
        raise SystemExit("unexpected output sha256 %s" % digest)
    with open(sys.argv[2], "wb") as out:
        out.write(data)
    print("%s  %s" % (digest, sys.argv[2]))


if __name__ == "__main__":
    main()
