# Outlast Requeue for Windows

Created by **whispersgone**.

The Windows release is a portable x64 application. It does not install a
service, create an automatic-start entry, or alter game display settings.
This build passed cross-compilation, parser/state tests, and binary API audits.
A real Windows 10/11 gameplay timeout test is still pending, so treat it as a
Windows beta until that test is reported.

## Install the PAK

1. Extract the complete archive to a normal folder. Keep the `payload` folder
   beside the install scripts.
2. Close The Outlast Trials.
3. Run `Install.bat`. Alternatively, start `Outlast Requeue.exe`, locate the
   game if necessary, and use **INSTALL PAK** while the game is closed.
4. If Steam discovery fails, open PowerShell in the extracted folder and pass
   the game directory explicitly:

   ```powershell
   .\Install.ps1 -GameDir "D:\SteamLibrary\steamapps\common\The Outlast Trials"
   ```

The installer verifies the bundled PAK by SHA-256 before copying it. The Steam
build number is only reported, not enforced: a game update no longer blocks
installation, because the PAK's exact hash is the real check. If a file with
the same name but a different hash already exists, installation stops without
overwriting it. When a Steam library is protected by Windows permissions, run
the script from an Administrator terminal; the script does not elevate itself.

## Run

1. Start The Outlast Trials and enter the Sleep Room.
2. Run `Outlast Requeue.exe` from the extracted folder.
3. Enable auto-requeue.
4. Select Imposter and start the first search yourself.

Keep the extracted folder intact. The application is portable; you may create a
normal shortcut to `Outlast Requeue.exe`, but no shortcut or automatic-start
entry is created for you.

Closing the window with the title-bar close button (X) exits the application
completely; there is no tray icon and nothing keeps running in the background.
While the window is open you may minimize it to the taskbar. Live search time is
shown in the window and is cumulative across every verified requeue in the
current chain.

## Remove the PAK

Close the game, then run `Uninstall.bat`. To specify a nonstandard library:

```powershell
.\Uninstall.ps1 -GameDir "D:\SteamLibrary\steamapps\common\The Outlast Trials"
```

The uninstaller deletes only a PAK whose SHA-256 exactly matches this release.
If the target has changed, it is left untouched and the script reports its
hash. Deleting the extracted application folder removes the portable app.

## Troubleshooting

- Game misbehaves after an update: the build number is no longer enforced, so
  the PAK stays installed across updates. If the Trial Board or a match breaks
  right after a game update, remove the PAK until a re-verified release is
  published.
- PAK hash mismatch: do not rename or replace the existing file automatically.
  Back it up and identify which version created it.
- Game not found: pass the folder containing `OPP` using `-GameDir`.
- Access denied: close the game and retry from an Administrator terminal.
- No active search detected: start one Imposter search manually. Only verified
  Invasion tickets are armed.
- Timeout detected but no new ticket: the application makes no blind retry.
  Return to the Trial Board and start a search manually.
- PC slept or the timeout log arrived late: the narrow action window expires
  and no delayed `Tab` or `F` is sent. Start another search manually.
- Target game window unavailable: do not run the game as Administrator. Windows
  blocks targeted messages between different privilege levels, and the app does
  not auto-elevate.
- SmartScreen warning: this executable is not code-signed. Verify `SHA256SUMS`
  and the release archive hash before deciding whether to run it.

The input path is deliberately narrow: the application posts `Tab` and `F`
only to a window whose process image matches the executable in the discovered
Outlast game folder.
It does not call foreground/focus APIs or global input APIs.
