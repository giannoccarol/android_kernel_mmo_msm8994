#!/usr/bin/env python3
"""Host tests for the card status parser of cityman-uim-bootstrap.

Builds the helper's source on the host with stub Android headers and checks
find_usim() on synthetic QMI UIM card status TLVs. No device access."""
from pathlib import Path
import struct
import subprocess
import tempfile

AID = bytes(range(0xA0, 0xB0))  # 16-byte placeholder, not a real card AID


def app(app_type, state, perso=2, pin1=2, aid=AID):
    return (bytes([app_type, state, perso, 0, 0, 0, len(aid)]) + aid +
            bytes([0, pin1, 3, 10, 0, 3, 10]))


def slot(present, apps):
    return bytes([1 if present else 0, 0, 0, 0, 0, len(apps)]) + b''.join(apps)


def status(gw_index, slots):
    return struct.pack('<4HB', gw_index, 0xFFFF, 0xFFFF, 0xFFFF, len(slots)) + b''.join(slots)


CASES = [
    # label, tlv, expected (found, slot, app, state, pin1)
    ("single USIM", status(0xFFFF, [slot(True, [app(2, 1)])]), (1, 0, 0, 1, 2)),
    ("USIM + ISIM", status(0x0000, [slot(True, [app(2, 7), app(5, 1)])]), (1, 0, 0, 7, 2)),
    ("ISIM before USIM", status(0xFFFF, [slot(True, [app(5, 1), app(2, 2, pin1=1)])]), (1, 0, 1, 2, 1)),
    ("index picks second USIM", status(0x0001, [slot(True, [app(2, 1), app(2, 4)])]), (1, 0, 1, 4, 2)),
    ("dual SIM, first slot empty", status(0xFFFF, [slot(False, []), slot(True, [app(2, 1)])]), (1, 1, 0, 1, 2)),
    ("no USIM", status(0xFFFF, [slot(True, [app(5, 1)])]), (0,)),
    ("card absent", status(0xFFFF, [slot(False, [app(2, 1)])]), (0,)),
    ("truncated", status(0xFFFF, [slot(True, [app(2, 1)])])[:20], (0,)),
    ("oversized AID", status(0xFFFF, [slot(True, [app(2, 1, aid=bytes(33))])]), (0,)),
]

DRIVER = r'''
#include <stdio.h>
#define main helper_main
#include "cityman-uim-bootstrap.c"
#undef main
int __android_log_print(int l, const char *t, const char *f, ...) { (void)l; (void)t; (void)f; return 0; }
int __system_property_get(const char *k, char *v) { (void)k; v[0] = 0; return 0; }
int main(int argc, char **argv)
{
	uint8_t buf[4096];
	size_t n = fread(buf, 1, sizeof(buf), stdin);
	struct usim u;
	unsigned int gw;
	(void)argc; (void)argv;
	if (!find_usim(buf, n, &gw, &u)) { puts("0"); return 0; }
	printf("1 %u %u %u %u\n", u.slot, u.app, u.state, u.pin1);
	return 0;
}
'''


def main():
    source = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="cityman-uim-test-") as temp:
        work = Path(temp)
        (work / "include/android").mkdir(parents=True)
        (work / "include/sys").mkdir()
        (work / "include/android/log.h").write_text(
            "#define ANDROID_LOG_INFO 4\n"
            "int __android_log_print(int, const char *, const char *, ...);\n")
        (work / "include/sys/system_properties.h").write_text(
            "#define PROP_VALUE_MAX 92\n"
            "int __system_property_get(const char *, char *);\n")
        (work / "driver.c").write_text(DRIVER)
        exe = work / "driver"
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-I", str(work / "include"), "-I", str(source),
                        str(work / "driver.c"), "-ldl", "-o", str(exe)], check=True)
        for label, tlv, expected in CASES:
            out = subprocess.run([str(exe)], input=tlv, capture_output=True, check=True).stdout
            got = tuple(int(x) for x in out.split())
            assert got == expected, (label, got, expected)
            print(f"PASS: {label}")
        print(f"{len(CASES)} parser checks passed; QMI transport not tested.")


if __name__ == "__main__":
    main()
