# Cityman camera bring-up

Tools and notes for the Lumia 950 XL (cityman) cameras on LineageOS 18.1.
They are not built or installed by the kernel Makefile. No vendor binaries,
calibration files or private logs belong here.

## Hardware, measured on the phone

| | Rear | Front |
|---|---|---|
| Module | Carrera (internal name Cooper) | Ducati |
| CCI master / slave (8-bit) | 1 / `0x20` | 0 / `0x20` |
| PHY / CSID | CSIPHY0 / CSID0 | CSIPHY2 / CSID2 |
| MCLK | MCLK0, 9.6 MHz | MCLK2, 9.6 MHz |
| Reset / standby GPIO | 92 / 91 | 104 / 105 |
| Rails | VAF L23 2.85 V, VANA L29 2.85 V, VDIG L25 1.15 V, VIO LVS1 | VANA L17 2.8 V, VIO LVS1 |
| Module ID `0x0000` (NVM) | `0xEACA`, revision `0x05` | `0x2140`, revision `0x00` |
| Silicon ID `0x0016` | `0x0230` (Sony IMX230) | `0x03BB` (ST VS6955) |
| Windows calibration | `0AEACA05.dcc` | `0A214000.dcc` |

Rails, GPIOs and clocks come from the Lumia ACPI tables (DSDT `CAMS`/`CAMF`).
The CCI masters and IDs were read with `cci_probe`. On master 0 the rear sensor
never answers, whatever the address, MCLK or GPIO 8/21 state.

## Kernel side

- `arch/arm64/boot/dts/mmo/common/camera-cityman.dtsi` replaces
  `qcom/msm8994-camera-sensor-mtp.dtsi`. The MTP file asked for L27 1.05 V,
  L17 2.7 V and L29 2.8 V, which the fixed cityman rails reject. It also
  claimed GPIO 93 (SiLabs FM reset) for a rear2 slot that does not exist, and
  put the front camera on CSI1.
- The front camera keeps `cell-index = 1` because the HAL looks for the front
  camera in slot 1. Its PHY/CSID indexes are 2.
- The AF actuator and OIS are on CCI master 1 together with the rear sensor
  (same module). Their slave addresses are still unknown.
- EEPROM nodes are not described: the Lumia module EEPROM addresses are unknown.
- `msm_sensor_match_id()` logs every ID it reads, with the expected ID and the
  slave address. `cci_probe` parses that line from `/dev/kmsg`.

The camera_v2 sensor driver is generic. Register tables, modes and the probe
power sequence come from the userspace `libmmcamera_<sensor>.so`.

## cci_probe

Freestanding static binary. It opens the `msm_sensor_init` subdev and sends
`CFG_SINIT_PROBE` with the cityman power sequence and expected ID 0, so an ACK
does not take the slot. It only reads from the sensor.

```bash
make CROSS_COMPILE=<kernel-mmo>/aarch64-linux-android-4.9/bin/aarch64-linux-android-
adb push cci_probe /data/local/tmp/
adb shell 'stop cameraserver; stop qcamerasvr'
adb shell /data/local/tmp/cci_probe 0 addr=0x20 reg=0x0016   # rear silicon ID
adb shell /data/local/tmp/cci_probe 1 addr=0x20              # front module ID
adb shell 'start qcamerasvr; start cameraserver'
```

Options: `mclk=Hz`, `addr=0xNN` (8-bit), `reg=0xNNNN`, `byte`/`word`,
`candidates`, `standby=0|1`, `reset=0|1`. Without `addr=` it scans every
address.

## make-imx230-lib.py

The ROM's camera HAL (`camera.msm8992.so`, bullhead) can only read the layout
of its own sensor libraries. The script rewrites the bullhead
`libmmcamera_imx377.so` from the phone into a probe library for the rear
IMX230: name, slave, ID register and value, and power sequence. It checks the
SHA-256 of the input and of the result.

```bash
adb pull /vendor/lib/libmmcamera_imx377.so
./make-imx230-lib.py libmmcamera_imx377.so libmmcamera_imx230.so
```

Result measured on 2026-10-24 with this library in `/vendor/lib`:

```
msm_sensor_match_id: imx230 read id 0x230 expected 0x230 slave 0x20
imx230 probe succeeded
```

The HAL then lists the rear module (`facing` 0, mount angle 270), and
`mm-qcamera-daemon` crashes in `CAM_startisp` (SIGSEGV). `dumpsys media.camera`
still reports 0 devices.

## What is left (userspace)

1. A camera HAL for MSM8994 (`camera.msm8994.so`) together with the VFE 4.6
   ISP modules (`libmmcamera_isp_*46.so`) from the same set. The `*46`
   modules do not load inside the bullhead HAL.
2. A real IMX230 library: register tables and modes for this module, 9.6 MHz
   MCLK. The IMX377 tables in the probe library cannot stream.
3. A library for the front ST VS6955 (`0x03BB`). No public library has the
   `0x2140` module.
4. AF/OIS/EEPROM addresses, then tuning from the Windows DCC files.
