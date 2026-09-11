# Outlast Requeue for Windows

Created by **whispersgone**.

The Windows release is a portable x64 application. It does not install a
service, create an automatic-start entry, alter game display settings, or
modify any game file.

## Run

1. Extract the archive to a normal folder.
2. Start The Outlast Trials and enter the Sleep Room.
3. Run `Outlast Requeue.exe`.
4. Enable auto-requeue.
5. Open the Terminal, select Imposter, and start the first search yourself.

The application finds the game through Steam. If that fails, press GAME FOLDER
and pick the folder that contains `OPP`.

## The widget

- Collapsed, it is a small strip: state, cumulative search timer, requeue
  count, and whether the game window is found.
- Rest the pointer on it to open the full panel with the status, attempt
  number, detected START position, buttons, game folder, and activity log. It
  collapses when the pointer leaves. The pin icon keeps it open.
- Drag it anywhere; the position is remembered. It never takes keyboard focus.
- It floats above other windows, including the game, while you queue. When a
  match is found it counts down ten seconds, then minimizes to the taskbar and
  stops floating. Restore it from the taskbar or enable auto-requeue to bring
  it back on top.
- The X icon exits the application completely.

There is no tray icon and nothing keeps running in the background after the
widget is closed. Live search time is cumulative across every verified requeue
in the current chain; it pauses while a requeue is in progress and stops only
when a match is found.

## Command line

- `--diagnostics` prints the discovered paths, the game build, whether the game
  window is found, and the START position it detects when the Trial Board is
  open on the TRIAL tab.
- `--self-test` runs the state engine and START detector checks.

## Troubleshooting

- Game not found: press GAME FOLDER and select the folder containing `OPP`.
- No active search detected: start one Imposter search manually. Only verified
  Invasion tickets are armed.
- START was not visible in the capture: the board opened on a page without the
  START bar, or no therapy was selected. Select Imposter on the TRIAL tab once;
  the board remembers it.
- Timeout detected but no new ticket: the application repeats the sequence up
  to three times, then waits for you. Return to the Trial Board and start a
  search manually.
- PC slept or the timeout log arrived late: the narrow action window expires
  and no delayed input is sent. Start another search manually.
- Target game window unavailable: do not run the game as Administrator. Windows
  blocks targeted messages between different privilege levels, and the app does
  not auto-elevate.
- SmartScreen warning: this executable is not code-signed. Verify `SHA256SUMS`
  and the release archive hash before deciding whether to run it.

The input path is deliberately narrow: the application posts `Tab` and one
mouse click only to a window whose process image matches the executable in the
discovered Outlast game folder. It never changes the foreground window and
never uses global input.
