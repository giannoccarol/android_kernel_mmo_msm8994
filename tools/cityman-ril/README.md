# Cityman Android RIL bring-up

This directory preserves the Android userspace fixes together with their kernel
base. It is not built or installed by the kernel Makefile. The reference platform
is Lumia 950 XL, LineageOS 18.1, existing Qualcomm vendor libraries and qmuxd.
No vendor binaries, firmware, private logs, SIM identifiers or PINs belong here.
See `Documentation/arm64/cityman-ril.txt` for the measured coexistence fix.
For ADSP firmware identity, reproducible extraction and outgoing-call results,
see `Documentation/arm64/cityman-adsp.txt`. ADSP ONLINE allowed the first call
to connect, but microphone and received audio remain broken.

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

## RIL startup / late PIN

The saved startup trace shows `libvss_nv_core.so` waiting 60.021 seconds for
the LG VSS NV service, then logging that the service is absent and returning -1.
The existing QCRIL calls its exported `qcci_qmi_vs_nv_init` through a PLT entry.
This is separate from standard UIM, NAS, DMS, WDS and modem NV storage.

`libcityman_ril_init.so` interposes only that initializer. With SoC ID 207 and
the exact boot property `ro.boot.dtb_idx=1`, it returns the same -1 immediately;
other/unknown identities forward to the original initializer. It does not
pretend that LG NV initialized successfully, patch vendor binaries, send QMI,
or read/write SIM credentials or NV. Boot properties avoid relying on root-only
`/proc/cmdline` from rild's radio UID.

For this Cityman vendor stack, add `libcityman_ril_init` to PRODUCT_PACKAGES
and use this rild-only init option (keep the existing data profile shim):

```rc
    setenv LD_PRELOAD /vendor/lib64/libcityman_ril_init.so:/vendor/lib64/libcityman_ds_target.so
```

Do not add the NV initializer shim to qmuxd, netmgrd or a global preload. It is
verified on two normal installed boots: the skip log appeared, the original
60-second LG retry loop did not, and SIM LOADED / Iliad LTE returned. Mobile
HTTPS and Wi-Fi traffic passed together after both boots. On the repeat
boot the owner confirmed a rapid PIN prompt. The log clock changed during
startup, so an exact before/after UI latency is not established. Restore the
original rild init option and reboot to roll back; the vendor NV library itself
remains untouched.

`python3 tools/cityman-ril/test-ril-init.py` performs eight host ELF interposition
checks using fake vendor exports and boot-property fixtures, including forwarding
on unknown/non-Cityman identities and leaving the vendor path unchanged without
preload. NDK r27c builds with `-Wall -Wextra -Werror` also pass. Neither is a
substitute for measuring Android boot and verifying LTE/Wi-Fi afterward.

## UIM and radio helper

The helper is Cityman-gated and does not enter a PIN. Its `--observe-only` option
does not activate/publish the SIM. Without options it exits at READY. The init
service uses `--activate-radio`: it stays alive, enables the primary NAS
subscription once per SIM-ready/radio-online transition, and retains its NAS/DMS
clients. It only queries DMS power state; it never forces DMS online or disables
Android airplane mode. The active-subscription mask preserves any existing bits.
The disabled init service starts when qmuxd runs and restarts after an exit.
An invalid command-line option is rejected. The fallback device check requires
SoC 207, the exact boot token `androidboot.dtb_idx=1`, and the scoped shim.
Support for multiple applications/cards and hotplug is not established. Recovery
after sustained modem failures needs further validation. Do not enable on Talkman.

## Registration and data: hardware results

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
CNSS's modem-status notification causes WLAN firmware to enable its WCI-2 UART.
Suppressing the notification in DT did not fix it. Explicitly sending the OFF
coexistence state to WLAN firmware on Cityman did: the #39 RAM test maintained
Iliad LTE and Wi-Fi simultaneously, mobile-interface HTTPS returned HTTP 200,
Wi-Fi ping passed 3/3, and no modem SSR appeared. Both cnd and cnss-daemon were
running. The actual modem remains on and its SSR tracking is unchanged. The
same kernel was then installed: a normal boot automatically restored SIM LOADED
and Iliad LTE, with concurrent mobile HTTPS 200 and Wi-Fi ping 3/3 and no SSR.

A spontaneous full-phone reboot also occurred after the #37 data test. The owner
confirmed it was not manual; pstore was empty after reboot, so its cause remains
unknown. Do not declare long-term stability from these short tests. The #39
airplane-mode test showed DMS low-power while enabled, then LTE and mobile HTTPS
returned after disabling airplane mode. No forced-online loop is used. The owner subsequently confirmed working SMS
and an outgoing call connecting after the ADSP firmware replacement documented
in `Documentation/arm64/cityman-adsp.txt`. Microphone and received audio remain
broken; incoming calls, long-duration operation and unplugged suspend are
unverified.

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
AT endpoint; they are not raw QMI messages. The verified Android path here uses UIM readiness, NAS primary activation
and Android radio power control, then the original netmgrd/IPA/WDS for data;
AT CFUN cycling is not required for this measured result. Do not substitute pmOS `/dev/wwan0qmi0`, raw readers of smdcntl0, or
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

## Revision of 2026-10-26 (branch test/cityman-ril-improve, not yet built)

- `cityman-uim-bootstrap.c` rewritten for review: named QMI message IDs, TLV
  offsets and states, kernel style.
- The card status parser walks every slot and application and picks the USIM
  the primary GW index names, or the first USIM on a present card. Multi-app
  cards (USIM + ISIM/CSIM) and dual-SIM layouts no longer stall silently;
  "No present USIM application" is logged once.
- Provisioning (0x0038) and Subscription OK (0x0040) are retried at most three
  times (5/15/45 s backoff), then only observed until the USIM state changes.
  Re-sending 0x0038 in a loop resets the SMD channel and drops the SIM.
- Once the radio subscription is handled and the state is stable, the helper
  polls every 30 s instead of 5 s. A failing DMS/NAS request reopens those
  clients instead of exiting.
- The helper runs as `radio` instead of root. All helpers share
  `cityman-detect.h`: `ro.boot.hardware=cityman`, or soc_id 207 with
  `ro.boot.dtb_idx=1`.
- `libcityman_ds_target` also answers `__system_property_get()`;
  `__system_property_read_callback()` readers remain uncovered.
- Host tests: `test-ril-init.py` (10 checks) and `test-uim-parse.py` (9 parser
  checks).

Open: on boot the PIN prompt appears a second time after a correct PIN and
then clears by itself. Check the radio log for the order of PIN verify,
Subscription OK and the next card status before changing the sequence.
