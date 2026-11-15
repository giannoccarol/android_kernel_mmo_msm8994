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


def status(gw_index, slots, gw_sec=0xFFFF):
    return struct.pack('<4HB', gw_index, 0xFFFF, gw_sec, 0xFFFF, len(slots)) + b''.join(slots)


# Dual SIM layout as an RM-1116 reports it (both cards present, the first USIM
# ready on the primary session, the second DETECTED with no secondary session),
# with placeholder AIDs.
DUAL = status(0x0000, [slot(True, [app(2, 7, pin1=3)]), slot(True, [app(2, 1, perso=0, pin1=0)])])
SECOND_ONLY = status(0xFFFF, [slot(False, []), slot(True, [app(2, 1)])])


CASES = [
    # label, tlv, expected (found, slot, app, state, pin1)[, index_off, slot_only]
    ("single USIM", status(0xFFFF, [slot(True, [app(2, 1)])]), (1, 0, 0, 1, 2)),
    ("USIM + ISIM", status(0x0000, [slot(True, [app(2, 7), app(5, 1)])]), (1, 0, 0, 7, 2)),
    ("ISIM before USIM", status(0xFFFF, [slot(True, [app(5, 1), app(2, 2, pin1=1)])]), (1, 0, 1, 2, 1)),
    ("index picks second USIM", status(0x0001, [slot(True, [app(2, 1), app(2, 4)])]), (1, 0, 1, 4, 2)),
    ("dual SIM, first slot empty", status(0xFFFF, [slot(False, []), slot(True, [app(2, 1)])]), (1, 1, 0, 1, 2)),
    ("no USIM", status(0xFFFF, [slot(True, [app(5, 1)])]), (0,)),
    ("card absent", status(0xFFFF, [slot(False, [app(2, 1)])]), (0,)),
    ("truncated", status(0xFFFF, [slot(True, [app(2, 1)])])[:20], (0,)),
    ("oversized AID", status(0xFFFF, [slot(True, [app(2, 1, aid=bytes(33))])]), (0,)),
    ("dual SIM: primary on slot 1", DUAL, (1, 0, 0, 7, 3), 0, 0),
    ("dual SIM: secondary on slot 2", DUAL, (1, 1, 0, 1, 0), 4, 1),
    ("dual SIM: secondary index names slot 2",
     status(0x0000, [slot(True, [app(2, 7)]), slot(True, [app(5, 1), app(2, 4)])], gw_sec=0x0101),
     (1, 1, 1, 4, 2), 4, 1),
    ("dual SIM, card in slot 2 only: no primary", SECOND_ONLY, (0,), 0, 0),
    ("dual SIM, card in slot 2 only: secondary", SECOND_ONLY, (1, 1, 0, 1, 2), 4, 1),
    ("dual SIM, card in slot 1 only: no secondary",
     status(0x0000, [slot(True, [app(2, 7)]), slot(False, [])]), (0,), 4, 1),
]

# Sessions as qcril leaves them after Android moved the LTE stack to SIM 2
# (RM-1116, build 82): primary GW on slot 2, secondary on slot 1, both USIMs in
# the personalization check with PIN disabled. Placeholder AIDs.
CROSSED = status(0x0100, [slot(True, [app(2, 4, pin1=3)]), slot(True, [app(2, 4, pin1=3)])],
                 gw_sec=0x0000)
EMPTY = status(0xFFFF, [slot(True, [app(2, 1, perso=0, pin1=0)]), slot(True, [app(2, 1, perso=0, pin1=0)])])
# Swap in progress: the secondary session still holds the slot 2 card.
HALF = status(0xFFFF, [slot(True, [app(2, 1, perso=0, pin1=0)]), slot(True, [app(2, 7, pin1=3)])],
              gw_sec=0x0100)

SESSION_CASES = [
    # label, tlv, (index_off, other_off, expected_slot), expected (found, slot, app, state, pin1)
    ("crossed: primary session drives slot 2", CROSSED, (0, 4, 1), (1, 1, 0, 4, 3)),
    ("crossed: secondary session drives slot 1", CROSSED, (4, 0, 0), (1, 0, 0, 4, 3)),
    ("crossed, stale stack property: still the named card", CROSSED, (0, 4, 0), (1, 1, 0, 4, 3)),
    ("empty, stacks swapped: primary gets slot 2", EMPTY, (0, 4, 1), (1, 1, 0, 1, 0)),
    ("empty, stacks swapped: secondary gets slot 1", EMPTY, (4, 0, 0), (1, 0, 0, 1, 0)),
    ("empty, stacks in order: primary gets slot 1", EMPTY, (0, 4, 0), (1, 0, 0, 1, 0)),
    ("swap in progress: primary takes the free card", HALF, (0, 4, 1), (1, 0, 0, 1, 0)),
    ("card in slot 2 only, stacks in order: no primary", SECOND_ONLY, (0, 4, 0), (0,)),
    ("card in slot 2 only, stacks in order: secondary", SECOND_ONLY, (4, 0, 1), (1, 1, 0, 1, 2)),
]

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define main helper_main
#include "cityman-uim-bootstrap.c"
#undef main
int __android_log_print(int l, const char *t, const char *f, ...) { (void)l; (void)t; (void)f; return 0; }
int __system_property_get(const char *k, char *v)
{
	const char *e = strcmp(k, "persist.radio.msim.stackid_0") ? NULL : getenv("STACKID0");
	v[0] = 0;
	if (!e) return 0;
	strcpy(v, e);
	return (int)strlen(v);
}
int main(int argc, char **argv)
{
	uint8_t buf[4096];
	size_t n;
	struct usim u;
	unsigned int gw;
	int ok;
	if (argc == 2 && !strcmp(argv[1], "primary-slot")) { printf("%u\n", primary_slot()); return 0; }
	n = fread(buf, 1, sizeof(buf), stdin);
	if (argc == 5 && !strcmp(argv[1], "session"))
		ok = find_session_usim(buf, n, (unsigned int)atoi(argv[2]), (unsigned int)atoi(argv[3]),
				       (unsigned int)atoi(argv[4]), &gw, &u);
	else
		ok = find_usim(buf, n, argc > 1 ? (unsigned int)atoi(argv[1]) : 0,
			       argc > 2 ? atoi(argv[2]) : -1, &gw, &u);
	if (!ok) { puts("0"); return 0; }
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
        for label, tlv, expected, *args in CASES:
            off, only = args if args else (0, -1)
            out = subprocess.run([str(exe), str(off), str(only)], input=tlv,
                                 capture_output=True, check=True).stdout
            got = tuple(int(x) for x in out.split())
            assert got == expected, (label, got, expected)
            print(f"PASS: {label}")
        for label, tlv, (off, other, slot_), expected in SESSION_CASES:
            out = subprocess.run([str(exe), "session", str(off), str(other), str(slot_)],
                                 input=tlv, capture_output=True, check=True).stdout
            got = tuple(int(x) for x in out.split())
            assert got == expected, (label, got, expected)
            print(f"PASS: {label}")
        import os
        for value, expected in ((None, 0), ("0", 0), ("1", 1), ("2", 0)):
            env = dict(os.environ)
            env.pop("STACKID0", None)
            if value is not None:
                env["STACKID0"] = value
            out = subprocess.run([str(exe), "primary-slot"], env=env, capture_output=True,
                                 check=True).stdout
            assert int(out) == expected, ("primary slot", value, out)
            print(f"PASS: stackid_0={value!r} -> primary session on slot {expected + 1}")
        total = len(CASES) + len(SESSION_CASES) + 4
        print(f"{total} parser checks passed; QMI transport not tested.")


if __name__ == "__main__":
    main()
