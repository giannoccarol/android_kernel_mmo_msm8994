# Cityman Android RIL bring-up

This directory preserves the Android userspace fixes together with their kernel
base. It is not built or installed by the kernel Makefile. The reference platform
is Lumia 950 XL, LineageOS 18.1, existing Qualcomm vendor libraries and qmuxd.
No vendor binaries, firmware, private logs, SIM identifiers or PINs belong here.

## Verified on hardware

- DPM opens DATA5_CNTL when the Qualcomm data services select the msm8994 profile.
  The ROM's global msm8992 platform property selects the wrong data path.
- Scope `LD_PRELOAD=/vendor/lib64/libcityman_ds_target.so` to **qmuxd, netmgrd
  and ril-daemon**. The shim changes only their `property_get("ro.board.platform")`
  result. Keep the global property unchanged: current graphics modules need it.
- The native helper activates the single primary USIM with UIM 0x0038, waits for
  PIN entry through Android, and sends Subscription OK (0x0040) only after PIN
  verification/disablement and personalization readiness. A complete runtime
  DETECTED -> PIN_REQUIRED -> READY -> Android LOADED cycle was measured.
  On the subsequent RAM boot it automatically reached PIN_REQUIRED.
- QMI queries use libqmi_cci and qmuxd, leaving `/dev/smdcntl0` under qmuxd's
  ownership. The 1024-byte aligned OS-parameter storage is specific to the
  existing vendor ABI; it is not a portable replacement for vendor headers.

## Build and integration

For the local Android 11 diagnostic build:

```sh
ANDROID_NDK_HOME=/path/to/android-ndk-r27c bash tools/cityman-ril/build.sh
```

`Android.mk` is an explicit device-tree integration option (not yet checked by
a full ROM build). Include it and add `libcityman_ds_target` and
`cityman-uim-bootstrap` to PRODUCT_PACKAGES. The library goes in vendor/lib64,
the helper in vendor/bin and its init file in vendor/etc/init. Add this service
option to each of qmuxd, netmgrd and ril-daemon in the device's init sources:

```rc
    setenv LD_PRELOAD /vendor/lib64/libcityman_ds_target.so
```

The helper is Cityman-gated and does not enter a PIN. Its `--observe-only` option
does not activate/publish the SIM. A subsequent helper exit at READY is expected.
An invalid command-line option is rejected. The fallback device check requires
SoC 207, the exact boot token `androidboot.dtb_idx=1`, and the scoped shim.
Support for multiple applications/cards, hotplug and modem restart recovery is
not established. Do not enable this service on Talkman.

## Registration and data: conditional hardware result

On both the RAM-only EFS-layout test kernel #37 and the unchanged installed
Wi-Fi kernel #30, the following diagnostic sequence registered to Iliad LTE:
Wi-Fi disabled, cnss-daemon/cnd temporarily stopped, SIM READY, NAS 0x004b with
TLV 0x14 and eight-byte primary mask 1. Android's original RIL/netmgrd then
established a data call on rmnet_data0 with address, route and DNS. Five Internet
pings and two DNS-based pings passed; an HTTPS GET on installed kernel #30
returned HTTP 200 over rmnet_data0. No APN changes or manual network setup were
needed. These are short functional results, not stability validation.

Enabling Wi-Fi while LTE is active reproduces modem SSR even with both daemons
stopped. The failure reason is
`coex_qmb.c:230:MCS_CXM assertion enc_rc == QMI_IDL_LIB_NO_ERR && out_buf_len >`.
The SSR clears the active mask, invalidates QMI clients and reinitializes the
SIM; it must not be misdiagnosed as a wrong PIN or merely a RIL mask override.
CNSS's modem-status notification causes WLAN firmware to enable its WCI-2 UART;
a Cityman-only DT test suppressing that notification is under investigation.

A spontaneous full-phone reboot also occurred after the #37 data test. The owner
confirmed it was not manual; pstore was empty after reboot, so its cause remains
unknown. Do not declare the RIL stable or fix the problem by permanently turning
off Wi-Fi. Do not repeatedly force NAS activation or DMS online in a service:
Android must retain airplane-mode control. Bootstrap restart recovery and a
complete persistent NAS activation service are still open work.

`qmi-client-query SERVICE MESSAGE [TLV_HEX [HOLD_SECONDS [POLL_MESSAGE]]]` can
keep one client open and poll a second request on that same client. Polling is
bounded and stops on an API error; no retrying radio activation loop is provided.

The pmOS implementation is a reference for the firmware sequence, not a
replacement for Android's transport or data stack:

- [ipa-netmgr modem lifecycle at 91a850b1](https://github.com/EpicLPer/ipa-netmgr/blob/91a850b1910f9f4d260dd472f1eff499ba49a9f5/src/modem.c)
- [Lumia EFS memory layout at 22986fd3](https://github.com/EpicLPer/linux/commit/22986fd32aac8eedb8fe2271e91f078496e95def)
- [UIM Subscription OK](https://www.repo.or.cz/libqmi.mirror.git/commit/a96371b6fc0541e2ee13df0d9e0e16cfbe23d0d4)

The pmOS sequence includes NAS primary activation, mode preference, DMS online,
then AT CFUN=0/1 and automatic network selection. Its AT commands use a separate
AT endpoint; they are not raw QMI messages. Android equivalents remain to be
validated with qmuxd for QMI and the confirmed DS AT port, then netmgrd/IPA/WDS
for data. Do not substitute pmOS `/dev/wwan0qmi0`, raw readers of smdcntl0, or
unverified AT-over-QMI messages.

The RAM-only #37 experiment boots MBA at 0x06300000 and metadata at
0x06400000, with EFS 0x06e80000/0x180000. It authenticated and booted, but the
subsequent successful #30 data test proves that relocation is not required for
this Android RIL result. Keep the original kernel memory layout as the baseline;
the relocation patch is archived locally and is not part of this source port.
No NV reset/restore is part of these fixes.

## Rollback

Keep a known working boot and copies of original init files before installing.
A reboot leaves a RAM test kernel and returns to the installed boot. Restore the
original qmuxd/netmgrd/ril-daemon init files and remove the helper init file to
disable userspace integration. Do not erase or restore modemst/fsg/fsc partitions
as a transport or registration workaround.
