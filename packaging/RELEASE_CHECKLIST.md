# Release checklist

- After every game update, re-verify the stock `TrialBoardTab` hashes against
  the new build and update the last-verified build ID.
- Re-audit the stock and patched `TrialBoardTab` hashes whenever the asset
  changes; re-derive the ubergraph offsets if it does.
- Run `make -C tests clean check`.
- Build Windows and run `tests/audit_windows_binary.sh`.
- Run Windows `--self-test`, `--ui-smoke-test`, and `--diagnostics`.
- Confirm the title-bar X exits the process completely (no lingering instance).
- Exercise sleep/resume and confirm an overdue action sends neither `Tab` nor
  `F`.
- Test install/uninstall in disposable default and secondary Steam libraries.
- Verify timeout → one `Tab → F` → new ticket on physical Windows 10 and 11.
- Confirm another foreground app remains foreground and game borderless/window
  settings remain byte-for-byte unchanged.
- Build archives with `packaging/build-release.sh` and verify all internal and
  outer SHA-256 manifests.
- Scan staged archives for personal paths, credentials, AES keys, caches, and
  extracted game assets.
- Note that the Windows executable is unsigned, or sign it before publication.
- Upload Windows and Source as separate Nexus files; do not claim mod manager
  support unless it has been explicitly implemented and tested.
