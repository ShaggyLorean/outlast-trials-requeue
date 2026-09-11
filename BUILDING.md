# Building Outlast Requeue

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

On Windows with a native MinGW-w64 install, pass an empty cross prefix:

```bash
mingw32-make -C src/windows clean all CROSS=
```

The result is `build/windows/OutlastRequeue.exe`. The Makefile also rejects a
binary containing any forbidden foreground, global-input, cursor-positioning,
or display-mode API name. The executable is a GUI-subsystem PE and requires no
separate runtime or helper process.

Run the built executable with `--self-test` for the engine and START detector
checks, `--ui-smoke-test` to create and destroy the window, and
`--diagnostics` to print what it discovers on this machine.

## Build release archives

After the Windows build:

```bash
./packaging/build-release.sh
```

The release builder starts from a clean Windows build, runs the C regression
suite, audits the Windows binary contract, verifies embedded version and build
constants, scans staged files for personal paths and credential-shaped values,
writes per-file SHA-256 manifests, and creates the Windows and source ZIP
archives. The source ZIP is assembled from an explicit allowlist and rejects
game asset files and symlinks.
