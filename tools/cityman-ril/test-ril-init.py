#!/usr/bin/env python3
"""Host ELF interposition tests; no ADB, vendor binaries, or device access."""
import os
from pathlib import Path
import subprocess
import tempfile


def run(*args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kwargs)


def main():
    source = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="cityman-ril-init-test-") as temp:
        work = Path(temp)
        (work / "include/android").mkdir(parents=True)
        (work / "include/sys").mkdir()
        (work / "include/android/log.h").write_text(
            "#define ANDROID_LOG_INFO 4\n"
            "int __android_log_print(int, const char *, const char *, ...);\n")
        (work / "include/sys/system_properties.h").write_text(
            "#define PROP_VALUE_MAX 92\n"
            "int __system_property_get(const char *, char *);\n")
        (work / "vendor.c").write_text(
            '#include <stdio.h>\n'
            'int qcci_qmi_vs_nv_init(void) { puts("vendor-called"); return 42; }\n')
        (work / "driver.c").write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int qcci_qmi_vs_nv_init(void);
int __system_property_get(const char *name, char *out)
{
    const char *value = getenv("CITYMAN_TEST_DTB_IDX");
    if (strcmp(name, "ro.boot.dtb_idx") || !value || strlen(value) >= 92) {
        out[0] = 0;
        return 0;
    }
    strcpy(out, value);
    return strlen(out);
}
int __android_log_print(int level, const char *tag, const char *format, ...)
{
    (void)level; (void)tag; (void)format;
    return 0;
}
int main(void) { printf("result=%d\n", qcci_qmi_vs_nv_init()); return 0; }
''')
        common = ("cc", "-Wall", "-Wextra", "-Werror")
        vendor = work / "libvss_nv_core.so"
        shim = work / "libcityman_ril_init.so"
        driver = work / "driver"
        soc = work / "soc_id"
        run(*common, "-shared", "-fPIC", str(work / "vendor.c"), "-o", str(vendor))
        run(*common, "-shared", "-fPIC", "-I", str(work / "include"),
            f'-DCITYMAN_SOC_ID_PATH="{soc}"', str(source / "cityman-ril-init.c"),
            "-ldl", "-o", str(shim))
        run(*common, str(work / "driver.c"), "-Wl,--export-dynamic",
            "-L", str(work), "-lvss_nv_core", "-o", str(driver))

        cases = [
            ("Cityman preserves unavailable result", "207\n", "1", True, -1),
            ("different SoC forwards vendor init", "251\n", "1", True, 42),
            ("different DT index forwards", "207\n", "2", True, 42),
            ("DT index matching is exact", "207\n", "10", True, 42),
            ("missing boot property forwards", "207\n", None, True, 42),
            ("malformed SoC forwards", "207 junk\n", "1", True, 42),
            ("missing SoC file forwards", None, "1", True, 42),
            ("not preloaded leaves vendor unchanged", "207\n", "1", False, 42),
        ]
        for label, soc_value, index, preload, expected in cases:
            if soc_value is None:
                soc.unlink(missing_ok=True)
            else:
                soc.write_text(soc_value)
            env = os.environ.copy()
            env["LD_LIBRARY_PATH"] = str(work)
            env.pop("LD_PRELOAD", None)
            env.pop("CITYMAN_TEST_DTB_IDX", None)
            if preload:
                env["LD_PRELOAD"] = str(shim)
            if index is not None:
                env["CITYMAN_TEST_DTB_IDX"] = index
            output = run(str(driver), env=env).stdout
            assert f"result={expected}\n" in output, (label, output)
            assert ("vendor-called" in output) == (expected == 42), (label, output)
            print(f"PASS: {label}")
        print("8 host checks passed; Android runtime/PIN timing not tested.")


if __name__ == "__main__":
    main()
