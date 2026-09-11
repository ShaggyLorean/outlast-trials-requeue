# Changelog

All notable changes to Outlast Requeue are documented here.

## 2.0.0 (2026-09-11)

The requeue works in a completely different way, nothing from the PAK era
survives, and the interface is new, so this is a major version.

- Verified hands-off on game build `25112110`: timeout record at 14:00:46.7,
  Trial Board open at +2.7 s, board ready at +4.0 s, START clicked at +4.5 s,
  board closed by the game at +5.2 s, replacement ticket at +12.4 s. The player
  was in another window the whole time and the pointer never moved. START was
  detected at 458, 1274 in a 2560 by 1440 client.
- The PAK is gone. Game build `25112110` (7.1) checks every mounted PAK
  against a whitelist and its anti-cheat reports any unknown one, so the
  patched `F` binding stopped being an option. Nothing in the game folder is
  touched any more, and there is no installer.
- The requeue now clicks the real START bar on the Trial Board. Posted mouse
  messages are dropped while the game believes it is in the background, so the
  sequence first tells the game it is active with the same messages Windows
  would post, clicks, and then tells it the opposite. The operating system
  foreground window never changes and the pointer never moves. Verified on a
  live client: timeout, Tab, board ready, click, and a new ticket six seconds
  later, with another application in the foreground the whole time.
- The START bar is located in a background capture of the game client instead
  of assumed at a fixed ratio, because the board is anchored to the window
  edges rather than scaled with it. The last detected position is remembered
  per client size, and a reference position is used only when no capture is
  possible.
- The first Trial selection is always manual. The board remembers it, so the
  tool only ever presses START; it never picks a therapy or a role.
- The interface is now a small floating widget instead of a window. Collapsed,
  it is a 320 by 64 strip with the state, the cumulative search timer, the
  requeue count, and whether the game window is found. Resting the pointer on
  it opens the full panel: status, attempt number, detected START position,
  the enable button, folder buttons, the game folder, and the activity log.
  It collapses again when the pointer leaves, unless pinned. Drag it anywhere;
  the position is remembered. It never takes keyboard focus.
- When a match is found the widget counts down ten seconds, then minimizes to
  the taskbar and stops floating above other windows. It also hides as soon as
  the game loads a trial map, armed or not, and comes back on its own when
  the Sleep Room loads, without taking focus from the game. Auto-requeue stays
  enabled across matches; start the next search yourself and it arms again.
- The search timer runs on the monotonic clock from the moment the searching
  record is seen. It used to compare the matchmaking service clock with the
  local clock, so a machine whose time was a few seconds behind showed 00:00
  for that long. It pauses while a requeue is in progress and continues from
  the same value when the new ticket is confirmed; it stops only on a match.
- `--diagnostics` reports the client size and the detected START position when
  the Trial Board is open, and `--self-test` covers the detector.
- Last verified game build is `25112110`.

## 1.0.3 (2026-08-18)

- Fixed the requeue landing on a disabled panel, which is why the sequence
  worked only occasionally. The Trial Board refuses input for the whole push
  transition, measured at 1.26 to 1.32 seconds on a live client, and the old
  fixed 1.2 second wait put `F` within milliseconds of that boundary. The
  sequence now waits for the game's own `Menu manager: enabling inputs` record
  and presses half a second after it.
- The delay between the timeout record and `Tab` went from 0.8 to 2.5 seconds.
  The client processes the timeout about one second after the record it comes
  from and spends another second on party cleanup, so the board used to be
  opened while the Invasion panel still believed it was searching.
- `Tab` is a toggle, and the board already being open meant the press closed it
  and `F` went to the Sleep Room. A pop record now triggers an immediate reopen.
- The game refuses `Tab` for some stretch after a matchmaking timeout, and no
  earlier release ever got past it: across every log on disk, the Trial Board
  only ever opened from a manual press. An unacknowledged `Tab` is now reposted
  every three seconds for two minutes instead of three times in nine seconds,
  and the give-up message reports whether the window was minimized and whether
  it had focus.
- An unconfirmed requeue is retried instead of ending the cycle. Stopping left
  the player with nothing in the queue at all while the app still looked armed.
- The confirmation window went from 15 to 25 seconds. Observed replacement
  tickets appear five to eight seconds after the press.
- The liveness check compared the matchmaking service clock with the local
  clock, so a machine whose time had drifted more than five seconds silently
  discarded every timeout and never requeued. It now measures against the
  game's own log prefix, which is written from the same machine.
- The action sequence no longer blocks the log reader, and it can no longer be
  abandoned between `Tab` and `F`, which used to leave the board open.
- Verified hands-off on game build `24382135`: two consecutive timeouts
  requeued without a single keystroke from the player. Board open at 2.77 and
  2.80 seconds after the timeout record, `F` at 4.74 and 4.67 seconds,
  replacement ticket at 12.10 and 11.47 seconds. The board reported itself
  ready 1.28 seconds after the push, which is exactly where the old fixed wait
  was aiming.

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
