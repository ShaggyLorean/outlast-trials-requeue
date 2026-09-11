# Release checklist

- After every game update, confirm the Trial Board still logs the
  `CharacterSheet_C` push/pop records and `Menu manager: enabling inputs`, and
  that `--diagnostics` still locates START with the board open.
- Run `make -C tests clean check`.
- Build Windows and run `tests/audit_windows_binary.sh`.
- Run Windows `--self-test`, `--ui-smoke-test`, and `--diagnostics`.
- Confirm the title-bar X exits the process completely (no lingering instance).
- Exercise sleep/resume and confirm an overdue action sends neither `Tab` nor
  a click.
- Verify timeout, `Tab`, click, new ticket on physical Windows 10 and 11.
- Confirm another foreground app remains foreground, the pointer does not
  move, and game borderless/window settings remain unchanged.
- Build archives with `packaging/build-release.sh` and verify all internal and
  outer SHA-256 manifests.
- Scan staged archives for personal paths, credentials, and game assets.
- Note that the Windows executable is unsigned, or sign it before publication.
- Upload Windows and Source as separate Nexus files; do not claim mod manager
  support unless it has been explicitly implemented and tested.
