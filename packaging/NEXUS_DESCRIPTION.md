# Outlast Requeue

Created by **whispersgone**.

Outlast Requeue restores an Invasion/Imposter search after the game's real
server-side matchmaking ticket times out. It watches `OPP.log`, arms the exact
active Invasion ticket, performs one targeted requeue after a matching timeout,
and requires a new ticket within 25 seconds.

It does **not** move you forward in matchmaking, change role probability,
modify regions, or contact matchmaking servers directly. Start the first
Imposter search manually.

## Files

- Windows x64, verified on build 24322931: the portable application, install
  and uninstall scripts, and the PAK.
- Source: controller and installer source plus tests. It contains no extracted
  game assets and no AES key.

## Safety contract

- No foreground/focus or game-window activation calls
- No global keyboard or mouse input
- No cursor movement
- No fullscreen, borderless, resolution, or display changes
- Retries only the ticket that timed out
- No delayed action after sleep/resume, a worker stall, or a stale log event
- No service and no automatic startup
- Exact PAK SHA-256 required

The app posts `Tab → F` only to the visible `UnrealWindow` whose process image
exactly matches the executable under the discovered game folder.

## Compatibility

- Steam App ID: `1304930`
- Last verified build ID: `24322931`
- PAK SHA-256:
  `1998125961ea66886ae41d71fe15ec2d555d045b980bc487ac5a6ea2a92d0c54`

The app no longer refuses on a new Steam build number; the exact PAK hash is
the gate. The PAK replaces `TrialBoardTab`, so it conflicts with any other mod
replacing that same asset. After a game update, remove it until the stock asset
has been re-verified against the new build.

## Use

1. Download the Windows archive and extract it completely.
2. Follow its `README.md` and run the included installer.
3. Start the game and enter the Sleep Room.
4. Start Outlast Requeue and enable auto-requeue.
5. Select Imposter and start the first search yourself.

Search time is cumulative across the entire verified requeue chain. The app
stops when an Invasion match succeeds. Closing the window (the title-bar X)
exits the application completely.

## Important notice

This is an unofficial community tool and is not affiliated with Red Barrels.
Rules, anti-cheat behavior, and mod policies can change; no third-party tool can
guarantee account safety. Verify the published archive checksum before use.
