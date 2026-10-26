# Lumia 950 XL / Cityman

- Camera exception requested on 2026-10-26: this worktree uses
  `lineage-20-cityman-camera`, created from `lineage-20-cityman` and populated
  with selected camera changes. Do not merge the Android 11 experiment stack
  wholesale. The old `lineage-18.1-cityman-camera` is preserved separately.
  Read `tools/cityman-camera/README.md` and the device source sensor's `ABI.md`
  for provenance, interoperability results and hardware limits before a phone
  test. The new module is experimental and is not installed by default.

- Work on `lineage-18.1-cityman-next`. The owner requested one branch on
  2026-10-25, superseding the earlier separate RIL branch convention. Keep changes
  in small commits; do not restore old experimental branches as the baseline.
- Earlier branches are recoverable from the complete local bundle
  `../archives/kernel-cityman-before-ril-20261025.bundle` and its branch manifest.
- Preserve the working QCA6174 RC1 core reset, Android SGMT firmware requirement
  and Lumia calibration. Read `Documentation/arm64/cityman-wifi.txt`.
- Retain the installed #30 modem memory layout. The EFS/MBA relocation
  experiment booted, but LTE data subsequently worked on #30 without it. Its
  patch is archived locally; do not present relocation as required for RIL.
- Android RIL sources and integration notes live in `tools/cityman-ril`. Preserve
  qmuxd ownership of smdcntl0. The scoped msm8994 shim and UIM activation /
  Subscription OK reach SIM LOADED. `--activate-radio` retains NAS/DMS clients
  and enables the primary subscription only when Android has the radio online.
- Cityman CNSS explicitly reports coexistence OFF to WLAN firmware, avoiding the
  coex_qmb.c:230 modem SSR seen with the incompatible WCI-2 UART exchange. This
  leaves actual modem power and SSR tracking unchanged; suppressing the DT
  notification alone did not work. RAM #39 passed simultaneous Wi-Fi/LTE data
  and airplane-mode recovery. Installed #39 also passed automatic SIM/LTE and
  concurrent mobile HTTPS / Wi-Fi traffic. Long-term stability is unverified;
  subsequent SMS/call results and remaining audio blockers are recorded below.
- For the measured absent LG VSS NV service, the separate rild-only initializer
  shim preserves its -1 result while skipping the 60-second timeout on Cityman.
  Two installed boots passed; the owner confirmed a rapid PIN prompt. Keep the
  original vendor NV library intact and retain forwarding on unknown boards.
- Never force DMS online periodically or bypass Android airplane-mode control.
- SMS work according to the owner's test. The original ADSP firmware failed
  authentication; replacing the complete set with the signed Cityman artifact
  brought ADSP ONLINE and allowed an outgoing call to connect. Microphone and
  received audio remain broken: no soundcard, missing msm-dai-q6-mi2s.3. Read
  Documentation/arm64/cityman-adsp.txt; do not fake PIL/ADSP readiness in RIL.
- Never commit toolchains, firmware/vendor binaries, boot images, private logs,
  NV backups or SIM identifiers. Keep raw diagnostics in the surrounding project.
- Back up and hash before changing device files. Prefer RAM boot tests, retain
  installed boot, and coordinate phone tests with the owner's current activity.
