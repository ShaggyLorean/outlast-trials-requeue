# Outlast Requeue

Created by **whispersgone**.

Outlast Requeue restores an Invasion/Imposter search after the game's real
server-side matchmaking ticket times out. It watches `OPP.log`, arms the exact
active Invasion ticket, opens the Trial Board and clicks START from the
background after a matching timeout, and requires a new ticket within 25
seconds.

It does **not** move you forward in matchmaking, change role probability,
modify regions, contact matchmaking servers directly, or touch any game file.
Start the first Imposter search manually.

## Files

- Windows x64, verified on build 25112110: the portable application and docs.
- Source: controller source plus tests. It contains no game assets.

## Safety contract

- No game files modified
- No change to the foreground window
- No global keyboard or mouse input
- No cursor movement
- No fullscreen, borderless, resolution, or display changes
- Retries only the ticket that timed out
- No delayed action after sleep/resume, a worker stall, or a stale log event
- No service and no automatic startup

The app posts `Tab` and a mouse click only to the visible `UnrealWindow` whose
process image exactly matches the executable under the discovered game folder.
The START bar is located in a background capture of the game client.

## Compatibility

- Steam App ID: `1304930`
- Last verified build ID: `25112110`

The app does not refuse on a new Steam build number. It depends on the game's
log records and the Trial Board layout only.

## Use

1. Download the Windows archive and extract it completely.
2. Start the game and enter the Sleep Room.
3. Start Outlast Requeue and enable auto-requeue.
4. Select Imposter and start the first search yourself.

The app is a small floating widget: state, search timer, and requeue count in
a strip, the full panel when the pointer rests on it. Search time is
cumulative across the entire verified requeue chain. When an Invasion match is
found the widget counts down ten seconds, then minimizes and stops floating;
it hides for every match and returns on its own in the Sleep Room. Auto-requeue
stays enabled across matches. The X icon exits the application completely.

## Important notice

This is an unofficial community tool and is not affiliated with Red Barrels.
Rules, anti-cheat behavior, and policies can change; no third-party tool can
guarantee account safety. Verify the published archive checksum before use.
