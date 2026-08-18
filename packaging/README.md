# Outlast Requeue 1.0.3

Created by **whispersgone**.

Outlast Requeue watches The Outlast Trials' own Invasion matchmaking log. When
the exact active Imposter ticket reports a timeout, it performs one targeted,
focusless requeue and waits for the game to confirm a new ticket.

This release does not change matchmaking priority, region selection, or server
behavior. It only restores the search that the game stopped after a real
timeout.

## Compatibility

- Game: The Outlast Trials on Steam (App ID `1304930`)
- Last verified game build: `24322931`
- PAK SHA-256:
  `1998125961ea66886ae41d71fe15ec2d555d045b980bc487ac5a6ea2a92d0c54`
- Windows: native x64 portable application

The PAK modifies a cooked Blueprint asset. After a game update, remove the PAK
until its stock asset has been re-verified against the new build; if the asset
is unchanged, the same PAK keeps working. The application does not gate on the
Steam build number, but it always refuses to automate when its required PAK
does not have the exact expected hash.

See `compatibility.json` for machine-readable compatibility metadata.

## Normal use

1. Install `zzz-OutlastRequeue_P.pak` using the platform instructions.
2. Start The Outlast Trials and enter the Sleep Room.
3. Start Outlast Requeue and enable auto-requeue.
4. Select Imposter and start the first search yourself.
5. Leave the application open or minimized. Closing it (the title-bar X) exits
   the application completely.

The first search is always manual. The application acts only after the game log
contains a matching `context=invasion`, `type=timed_out` event for the exact
ticket it armed. Replayed or stale search/success/cancel events are ignored. It
stops after a confirmed Invasion match succeeds.

## Safety boundaries

Outlast Requeue:

- never activates, raises, or focuses the game window;
- never uses global keyboard or mouse input;
- never moves the pointer;
- never changes fullscreen, borderless, resolution, or display settings;
- never retries without a new matching timeout and ticket confirmation;
- never sends a delayed action after its short safe timing window expires;
- never installs a background service or automatic-start entry.

Input messages are posted only to the verified Outlast game window, which is
matched by process image identity before anything is posted.

## What the PAK does

The PAK does not monitor matchmaking and cannot requeue by itself. It redirects
the existing Trial Board `F` action to the real Invasion start path. The desktop
application detects the exact timeout, posts targeted `Tab`, waits for the
game to report that the Trial Board is accepting input again, and only then
invokes the patched `F` action. A newly created Invasion ticket must appear in
the game log within the 25-second confirmation window.

## Platform instructions

Windows only: read `README.md` in the Windows archive.

## Online-game notice

This is an unofficial community tool and is not affiliated with or endorsed by
Red Barrels. Game updates, matchmaking changes, platform rules, and
anti-cheat/mod policies can change. No release can guarantee account safety;
review the current rules and use third-party modifications at your own risk.

## Licensing

`LICENSE-CODE` applies only to original Outlast Requeue code. The modified game
asset, game icon/artwork, The Outlast Trials, and third-party components are not
licensed under that file. See `THIRD_PARTY_NOTICES.md`.
