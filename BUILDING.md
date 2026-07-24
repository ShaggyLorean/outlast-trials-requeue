# Building Outlast Requeue

The release is split into original controller code and a build-locked modified
game asset. `LICENSE-CODE` covers only the original controller code.

## Test the platform-neutral state engine

```bash
make -C tests clean check
```

The test executable exercises exact-ticket matching, stale timeout rejection,
cumulative timing, recovery scans, success/cancel/disconnect handling, and the
one-shot verification timeout.

## Build Windows x64

Install a MinGW-w64 x86-64 toolchain, then run:

```bash
make -C src/windows clean all
```

The result is `build/windows/OutlastRequeue.exe`. The Makefile also rejects a
binary containing any forbidden foreground, global-input, cursor-positioning,
or display-mode API name. The executable is a GUI-subsystem PE and requires no
separate runtime or helper process.

## Build release archives

After the Windows build:

```bash
./packaging/build-release.sh
```

The release builder starts from a clean Windows build, runs the shared C
regression suite, audits the Windows binary contract, verifies embedded
version/hash constants, scans staged files for personal paths and
credential-shaped values, writes per-file SHA-256 manifests, and creates the
Windows and source ZIP archives. The source ZIP is assembled from an explicit
allowlist and rejects PAK/UAsset/UExp files, symlinks, and extracted-asset
staging directories.

## PAK provenance and game updates

The distributed PAK cannot be rebuilt from this source archive alone because
the archive intentionally contains no extracted copyrighted game assets and no
game encryption key. See `src/pak/README.md`. Obtain the asset from a lawfully
installed copy of the game and verify every pinned hash before patching.

After a game update, check the asset again before trusting the old PAK. Extract
the stock `TrialBoardTab` pair from the new build and hash it. If the hashes
still match the pinned values, the existing PAK stays valid and only the
last-verified build ID needs updating. If either hash differs, derive the
ubergraph offsets in `src/pak/patch_trialboard_requeue.py` again. The offsets
are recompiled per build, so the old constants point at the wrong handler.
