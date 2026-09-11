# Outlast Requeue 1.0.4

Created by **whispersgone**.

Outlast Requeue watches The Outlast Trials' own Invasion matchmaking log. When
the exact active Imposter ticket reports a timeout, it opens the Trial Board,
clicks START from the background, and waits for the game to confirm a new
ticket. No game file is modified.

This release does not change matchmaking priority, region selection, or server
behavior. It only restores the search that the game stopped after a real
timeout.

## Compatibility

- Game: The Outlast Trials on Steam (App ID `1304930`)
- Last verified game build: `25112110`
- Windows: native x64 portable application

The application does not gate on the Steam build number. It depends only on
the game's log records and on the Trial Board layout, so a game update that
changes neither leaves it working.

See `compatibility.json` for machine-readable compatibility metadata.

## Normal use

1. Start The Outlast Trials and enter the Sleep Room.
2. Start Outlast Requeue and enable auto-requeue.
3. Open the Terminal, select Imposter, and start the first search yourself.
4. Leave the application open or minimized. Closing it (the title-bar X) exits
   the application completely.

The first search is always manual. The board remembers the selection, so the
application only ever presses START. It acts only after the game log contains
a matching `context=invasion`, `type=timed_out` event for the exact ticket it
armed. Replayed or stale search, success, or cancel events are ignored. It
stops after a confirmed Invasion match succeeds.

## How the click works

The application posts `Tab` to the game window, waits for the game to log
that the Trial Board accepts input, captures the game client in the background,
locates the START bar in that capture, and posts a mouse click at it. The game
only routes mouse messages while it believes it is the active application, so
it is told that it is, by message, for the duration of the click, and told the
opposite afterwards. Which window Windows considers foreground never changes,
and the pointer never moves.

## Safety boundaries

Outlast Requeue:

- never modifies, adds, or removes game files;
- never changes which window is in the foreground;
- never uses global keyboard or mouse input;
- never moves the pointer;
- never changes fullscreen, borderless, resolution, or display settings;
- never retries without a new matching timeout and ticket confirmation;
- never sends a delayed action after its short safe timing window expires;
- never installs a background service or automatic-start entry.

Input messages are posted only to the verified Outlast game window, which is
matched by process image identity before anything is posted.

## Online-game notice

This is an unofficial community tool and is not affiliated with or endorsed by
Red Barrels. Game updates, matchmaking changes, platform rules, and
anti-cheat policies can change. No release can guarantee account safety;
review the current rules and use third-party tools at your own risk.

## Licensing

`LICENSE-CODE` applies only to original Outlast Requeue code. The game
icon/artwork, The Outlast Trials, and third-party components are not licensed
under that file. See `THIRD_PARTY_NOTICES.md`.
