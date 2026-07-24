# Changelog

All notable changes to Outlast Requeue are documented here.

## 1.0.2 (2026-07-22)

- Added support for The Outlast Trials build `24322931`. The stock
  `TrialBoardTab.uasset` and `TrialBoardTab.uexp` were extracted from the new
  build and are byte-for-byte identical to build `24226502`, so the hash-pinned
  PAK is unchanged and keeps the same SHA-256.
- Rebuilt the Windows interface. The window is painted directly and
  double-buffered, so it no longer flickers. It shows a state pill (STANDBY or
  AUTO-REQUEUE ARMED), a large session timer, the current status, and a
  monospaced activity panel.
- Buttons now have hover and pressed states, a hand cursor, and icon glyphs on
  the three secondary actions.
- The title bar, window corners, and border follow the Windows 11 dark style,
  and the manifest declares Common Controls v6.
- Removed the strict Steam build-ID check. A game update no longer locks the
  app out when it changes nothing this tool touches. The exact PAK SHA-256 is
  still enforced.
- Removed the system tray. The title-bar close button now exits the application
  instead of hiding it.
- The activity panel keeps the most recent 400 lines instead of growing for the
  whole session.
- Dropped the Linux/Proton build. Outlast Requeue is Windows-only, and the
  Linux controller, helper, and packaging are gone.
- The automation is unchanged from 1.0.1: same state engine, same safety
  boundaries, same PAK verification, same focusless `Tab` → `F` requeue path.

## 1.0.1 (2026-07-16)

- Added support for The Outlast Trials hotfix build `24226502`.
- Verified that the hotfix's stock `TrialBoardTab.uasset` and
  `TrialBoardTab.uexp` are byte-for-byte identical to build `24162678`, so the
  existing hash-pinned PAK remains unchanged.

## 1.0.0 (2026-07-16)

- Added visible creator attribution for **whispersgone** across both apps and
  release metadata.
- Added the verified Invasion ticket state machine.
- Added exact-ticket timeout detection and new-ticket confirmation.
- Added stale terminal/duplicate event rejection and fail-closed action expiry
  for delayed logs, worker stalls, and sleep/resume.
- Added cumulative search timing across the complete requeue chain.
- Added the targeted `Tab` → patched `F` requeue sequence (with Linux-only
  stale-modifier cleanup inside the Proton game window).
- Added the portable native Windows x64 application and tray controller.
- Added the Linux/Proton GTK 3 and AppIndicator controller.
- Added a shared, hash-pinned Blueprint PAK for game build `24162678`.
- Added hash-protected Windows and Linux install and uninstall scripts.
- Added exact Proton-runtime selection and ownership-safe Linux removal.
- Documented the no-focus, no-global-input, no-cursor, and no-display-change
  automation contract.

Windows native hardware validation remains required before labeling this build
as tested on physical Windows hardware. The release metadata records this
explicitly; cross-build or compatibility-layer checks are not represented as a
native hardware test.
