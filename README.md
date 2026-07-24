# Outlast Requeue

Restarts an Invasion search in The Outlast Trials after the game's own
matchmaking ticket times out, without stealing focus, moving your cursor, or
touching global input.

![The application in standby and in its armed state](docs/screenshot.png)

Created by whispersgone. Version 1.0.2, Windows x64.

## The problem

When you queue as Imposter and the server times the ticket out, the game stops
searching and drops you back to the Trial Board. If you are not watching the
screen, the queue is simply dead until you notice.

Outlast Requeue watches the game's own log, recognises the timeout for the
exact ticket it armed, and starts the search again once. Then it waits for the
game to confirm that a new ticket actually exists.

## How it works

The application never guesses at game state. It reads
`%LOCALAPPDATA%\OPP\Saved\Logs\OPP.log`, the log the game already writes, and
tracks one Invasion ticket at a time:

1. You start the first Imposter search yourself. The application sees the
   `context=invasion` search event and arms that exact ticket ID.
2. When a `type=timed_out` event arrives for that same ticket, and only then,
   it posts `Tab` to the game window, waits for the timeout interface, and
   posts `F`.
3. A new Invasion ticket has to appear in the log within 15 seconds. If it does
   not, the application stops and waits for you. It never retries blindly.

Both key messages go to one specific window: the visible `UnrealWindow` whose
process image matches the executable inside the game folder it discovered. The
game is never activated, raised, or focused.

The `F` action works because of a small PAK that redirects the existing Trial
Board `F` binding to the Invasion start path. The PAK changes one operand in
one cooked Blueprint asset and cannot requeue anything by itself.

## Requirements

| Item | Value |
| --- | --- |
| Game | The Outlast Trials on Steam, App ID `1304930` |
| Last verified game build | `24322931` |
| System | Windows x64 |
| PAK SHA-256 | `1998125961ea66886ae41d71fe15ec2d555d045b980bc487ac5a6ea2a92d0c54` |

The application does not refuse to run on a newer Steam build number, because
that number changes on every hotfix even when nothing this tool touches has
changed. What it does enforce is the PAK hash. If the installed PAK is not the
exact expected file, it will not automate.

## Installing

Download the Windows archive, extract all of it, close the game, and run
`Install.bat`. The installer verifies the bundled PAK by SHA-256 before copying
it, and refuses to overwrite a file of the same name with a different hash.

If Steam discovery fails, point the script at the folder yourself:

```powershell
.\Install.ps1 -GameDir "D:\SteamLibrary\steamapps\common\The Outlast Trials"
```

Then start the game, enter the Sleep Room, run `Outlast Requeue.exe`, enable
auto-requeue, and start the first Imposter search manually. Closing the window
with the title-bar X exits the application completely. There is no tray icon
and nothing keeps running in the background.

Full instructions, including removal and troubleshooting, are in
[packaging/windows/README.md](packaging/windows/README.md).

## Safety boundaries

Outlast Requeue:

- never activates, raises, or focuses the game window;
- never uses global keyboard or mouse input;
- never moves the pointer;
- never changes fullscreen, borderless, resolution, or display settings;
- never retries without a new matching timeout and ticket confirmation;
- never sends a delayed action after its short safe timing window expires;
- never installs a background service or automatic-start entry.

The build checks these claims. `tests/audit_windows_binary.sh` reads the
compiled binary's import table and fails if it finds any foreground,
global-input, cursor, or display-mode API. The same names are rejected in the
source.

## Building

Install a MinGW-w64 x86-64 toolchain, then:

```bash
make -C src/windows clean all
```

The result is `build/windows/OutlastRequeue.exe`, a GUI-subsystem PE64 that
needs no separate runtime. The state engine has its own test suite:

```bash
make -C tests clean check
```

See [BUILDING.md](BUILDING.md) for release archives and asset provenance.

## Repository layout

```
src/windows/   native Win32 application
src/common/    platform-neutral ticket state engine, shared with the tests
src/pak/       patch script and the pinned asset hashes
packaging/     installer scripts, release builder, and documentation
tests/         engine test suite and the binary contract audit
```

## After a game update

Steam bumps its build number on every hotfix, so the number alone tells you
nothing. Check the asset instead. Extract the stock `TrialBoardTab` pair from
the new build and hash it.

If the hashes still match the values pinned in
[src/pak/README.md](src/pak/README.md), the existing PAK is still correct. If
they differ, the ubergraph offsets have to be derived again, because they are
recompiled per build and the old constants point at the wrong handler.

## Online-game notice

This is an unofficial community tool and is not affiliated with or endorsed by
Red Barrels. Game updates, matchmaking changes, platform rules, and
anti-cheat or mod policies can change. No release can guarantee account safety.
Review the current rules and use third-party modifications at your own risk.

## Licensing

`LICENSE-CODE` covers the original Outlast Requeue code only. The modified game
asset, the game icon and artwork, The Outlast Trials itself, and third-party
components are not licensed under that file. See
[THIRD_PARTY_NOTICES.md](packaging/THIRD_PARTY_NOTICES.md).
