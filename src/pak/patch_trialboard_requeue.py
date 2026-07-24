#!/usr/bin/env python3
"""Build a byte-for-byte-safe TrialBoardTab patch for focusless requeue.

The existing Menu_TrialBoard_LeaveLobby input listener calls the 18-byte
OnLeaveLobbyKeyPressed wrapper. This patch changes only that wrapper's
ExecuteUbergraph entry point from 2971 (leave lobby) to 2878 (the existing
StartPVPMatchBtn OnClicked / StartMatchCountdown implementation). No bytecode
is inserted or removed, so package offsets and jump targets remain unchanged.
"""

from __future__ import annotations

import hashlib
import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "extracted/OPP/Content/Widgets/UI/CharacterSheet"
DEST = ROOT / "mod-staging/OPP/Content/Widgets/UI/CharacterSheet"

EXPECTED_UASSET_SHA256 = "434f350c4d47ed7168747d66c68bda0a23c5fe94927267713d7d9b7d6aa39e56"
EXPECTED_UEXP_SHA256 = "dd75dc715cd2dc71270485b82b9139066561500aaca32f884e75fd41022518b0"

# EX_LocalFinalFunction, StackNode=3, EX_IntConst, target, EndFunctionParms,
# EX_Return, EX_Nothing, EX_EndOfScript. The target is the only changed field.
OLD = bytes.fromhex("46 03 00 00 00 1d 9b 0b 00 00 16 04 0b 53")
NEW = bytes.fromhex("46 03 00 00 00 1d 3e 0b 00 00 16 04 0b 53")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    source_uasset = SOURCE / "TrialBoardTab.uasset"
    source_uexp = SOURCE / "TrialBoardTab.uexp"
    uasset = source_uasset.read_bytes()
    uexp = source_uexp.read_bytes()

    if sha256(uasset) != EXPECTED_UASSET_SHA256:
        raise SystemExit("Unexpected TrialBoardTab.uasset; refusing to patch")
    if sha256(uexp) != EXPECTED_UEXP_SHA256:
        raise SystemExit("Unexpected TrialBoardTab.uexp; refusing to patch")
    if uexp.count(OLD) != 1:
        raise SystemExit(f"Expected exactly one leave-lobby wrapper; found {uexp.count(OLD)}")

    patched = uexp.replace(OLD, NEW, 1)
    if len(patched) != len(uexp):
        raise SystemExit("Patch changed asset length; refusing to continue")

    DEST.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source_uasset, DEST / "TrialBoardTab.uasset")
    (DEST / "TrialBoardTab.uexp").write_bytes(patched)

    print(f"original_uexp_sha256={sha256(uexp)}")
    print(f"patched_uexp_sha256={sha256(patched)}")
    print(f"changed_bytes={sum(a != b for a, b in zip(uexp, patched))}")


if __name__ == "__main__":
    main()
