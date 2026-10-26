# Cityman camera on Android 13

The active branch is `lineage-20-cityman-camera`, created directly from
`lineage-20-cityman` at `5827d9a6f12` on 2026-10-26. The Android 11 camera
branch remains an archive of the earlier trials. This branch does not merge
that whole experimental stack into Android 13.

## Implemented baseline

- Cityman rear/front wiring replaces the Qualcomm MTP sensor description.
  The rear uses CCI1, MCLK0 and CSIPHY0/CSID0; front uses CCI0, MCLK2 and CSI2.
- `camera_v2` and its exported headers match the angler Oreo driver used with
  the complete angler MSM8994/VFE 4.6 vendor stack.
- Rear CSI lane assignment is the measured `0x0423`. CSI setup and release
  counters are logged to distinguish missing data from corrupt packets.
- The 104-byte angler buffer-manager compat ABI is checked at compile time.
  Native translation state is initialized and flags are copied explicitly.
- Front and unvalidated AF/OIS devices stay disabled; the rear does not link
  unverified companion drivers. The front die's commercial part number is
  unconfirmed; only module/die IDs have been established.

No Motorola address remap, 24 MHz clock override, legacy 100-byte buffer ABI,
ISP44 register override or Android 6 HAL wrapper was imported. The changes
outside the camera driver, relevant camera headers and board description are
limited to these tools and documentation. Audio, modem and Android 13 backports
remain on the Android 13 baseline.

## Build result

The kernel at `d62fca264ee0ea3b4acd54caa93316cc27886f55` compiled successfully
with Android GCC 4.9 and `mmo_defconfig`: zero errors, 81 warnings inherited
outside `camera_v2`, zero warnings in camera sources. Image.gz-dtb SHA-256:

`2ebfe9d11d2a10c54e9d73fff2b50c77bcf2db93f78ce04156de82260b9ed61a`

Private output: `android-cityman/kernel-camera-a13-20261026/angler-baseline`
in the surrounding workspace. Source commit, toolchain, full config and image
hashes are saved alongside it. Later documentation/reference-inspection changes
do not alter the compiled camera code.

From a clean checkout, build into a new path without spaces:

```sh
tools/cityman-camera/build-a13-kernel.sh NEW_OUTPUT /path/to/aarch64-linux-android- 8
```

The script archives the committed inputs to avoid contaminating another
worktree's generated objects. It creates no boot image and never accesses the
phone. No phone modifications or flash were performed during this preparation;
the owner is working on reboot issues separately.

The Android 13 HAL/module build also passed using isolated source archives:
device `261de03`, kernel `d62fca264ee`, vendor `8e70346`. The vendor's 131 camera
file hashes all match its manifest after removal of the old probe module.
Full inputs/logs/output hashes are private under
`android-cityman/camera-a13-hal-20261026`. The device branch documents its build
wrapper's compiler-cache and output-hash-path repairs. No source HAL changes
were needed relative to the Android 13 device baseline.

## SMIA++ integration boundary

Update 2026-10-26: on the owner's request, a new equivalent source plugin has
been implemented in the device camera branch's `camera-sensors/imx230`.
The Android-built shared object passed ARM32 tests against the actual pinned
angler consumer. The original hraj source remains missing, but no longer blocks
an independent experimental implementation. See that directory's `ABI.md`.

`inspect-smia-reference.py` reads the pinned public reference without executing
its scripts or touching a phone. Fetch the reference if it is not already in
the Git object database:

```sh
git fetch https://github.com/hraj9258/android_kernel_mmo_msm8994 talkman-cam-clean
python3 tools/cityman-camera/inspect-smia-reference.py
```

The verified first rear mode is DCC mode 4: 2496x1872, four-lane uncompressed
RAW10 (CSI DT 0x2b), MCLK 9.6 MHz, EXTCLK 0x0999, line length 6024, frame
length 2550, VT pixel clock 460.8 MHz, nominal 29.99766 fps. The public arrays
include zero sentinels; these are not writes to the read-only module-ID register.
Matching metadata in the sensor library is required before enabling any program.

The reference inspection tool does not implement a driver. The new device
plugin uses the generic sensor driver instead of importing the full SMIA++
kernel wrapper: the original angler consumer accepts long pointer-based
register records, as verified with its actual conversion function. Its public
12-entry inline exposure record is a different interface and is not a limit
on these init/mode tables. Front Ducati remains disabled.

The next implementation must define sensor metadata, mode/CSI selection,
exposure/gain and power/configure/start/stop together. Keep sensor state per
device, honor stop/start, check CCI errors, and derive ISP46 geometry from the
matching sensor mode. Do not port the ISP44 overrides or global `keep_stream`
workaround to compensate for an inconsistent userspace descriptor.

The first hardware acceptance is an actual frame returned through the HAL;
camera enumeration, SOF metadata and CSI packet counts alone are insufficient.
Preview/open/close, JPEG, full resolution, 3A, AF/OIS/flash and front capture
then need their own measured results.

## Source module build and integration (2026-10-26)

The camera branches include the owner's latest main Android 13 commits:
kernel through `85514ba37fc`, device through `fb0314c`, vendor at `017bf72`.
The kernel merge `dbef37116b8` compiled successfully; its private output is
`android-cityman/kernel-camera-a13-20261026/source-sensor-base`, image SHA-256
`7ffa4d330e39624897bbadbfbd1b67c16fa8a7691b3c1695c7123d7b69a625de`.

The Android source-module build used device `4b6259b`, kernel `dbef37116b8`,
vendor `8e70346`. The IMX230 library SHA-256 is
`849a6f2365f3b6fad30859c771694bb9ddab93fa8a4f76b31730eba9c40a14bd`.
The actual shared object passed the original angler capability/format and
I2C/power/CSI translation functions under QEMU, with an official IMX377 control
and a rejected zero-mode negative control. Separate tests cover new exposure
callbacks, frame bounds, group hold and output buffer guards.

No confidential SDK header or modified input sensor binary is used to build
the library. Untested extension records are disabled. Product installation is
gated by `CITYMAN_CAMERA_SMIA_EXPERIMENTAL=true`. Address 0x20 programming,
shared LVS1 ownership, streaming and image delivery remain hardware questions;
the current angler chromatix seed and optical metadata are provisional.
The owner is handling audio/network/reboot work, so no phone was modified here.

## Source search (2026-10-26)

- All 11 public kernel heads from hraj9258 were fetched under read-only
  reference names `hraj-source/*`. Searching their reachable history for
  `sensor_lib_imx230.c` or `senslib` found no file history. Branch checkout and
  active sources were not changed by this search.
- The public device branches, repository releases and user gists also did not
  supply the module. GitHub code searches for the exact filename/path/token,
  plus issue and commit searches, returned no matches. Search indexes are not
  proof that a file has never existed elsewhere.
- EpicLPer's `Lumia950-Camera-Bringup` at
  `e7a567de35021900e46b4567cddf779ca530ce77` supplies older overlay notes and
  `scripts/pack-imx230.py`. That script edits an IMX377 binary; it is not the
  missing C source. Its August test-generator status must not be confused with
  the later live-sensor work in the September/October reference.
- Generic `imx230_lib.c` files in Qualcomm vendor source mirrors have a
  different filename/provenance and do not establish the Lumia sensor ABI or
  hraj9258's implementation. None was imported.

Possible useful author request (not sent):

> Puoi pubblicare `senslib/sensor_lib_imx230.c`, gli header/build usati e le
> revisioni esatte di device/vendor/HAL/daemon del tuo `talkman-cam-clean`?
> Nel kernel troviamo i programmi SMIA++, ma manca il modulo userspace per
> riprodurre il risultato. Per la frontale hai un altro branch? In quello
> pubblico Ducati risulta ancora disabilitata.

## Credits and sources

- Android4Lumia950: djokcho, EpicLPer, j0sh1x and community, original Lumia
  kernel/device work: https://github.com/Android4Lumia950/android_kernel_mmo_msm8994
- Google angler camera driver, `android-msm-angler-3.10-oreo-m7`
  `309d642245e2`: https://android.googlesource.com/kernel/msm/+/309d642245e2
- archienz, lane assignment and sensor identification, device tree
  `e33ff2c731ea`: https://github.com/archienz/android_device_msft_talkman
- hraj9258, SMIA++ mode/clock reference `6ab0e0e9018`, including initial sensor
  work `4b0627fff15`, VT-clock findings and assertion `138aff8b371`:
  https://github.com/hraj9258/android_kernel_mmo_msm8994/tree/talkman-cam-clean
- EpicLPer, earlier overlay and source-search lead, `e7a567de3502`:
  https://github.com/EpicLPer/Lumia950-Camera-Bringup
- LineageOS and LineageOS-UL, source HAL and Android 13 framework integration.
