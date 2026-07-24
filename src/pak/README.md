# PAK patch source

`patch_trialboard_requeue.py` documents and applies the build-locked, one-byte
`TrialBoardTab.uexp` change used by Outlast Requeue. It expects locally
extracted source files at:

```text
extracted/OPP/Content/Widgets/UI/CharacterSheet/TrialBoardTab.uasset
extracted/OPP/Content/Widgets/UI/CharacterSheet/TrialBoardTab.uexp
```

The script refuses an unexpected source hash, requires exactly one matching
wrapper, preserves file length, and prints the resulting hashes. It does not
contain an AES key, extracted game data, or a repacking utility.

For the supported Steam build `24322931` (whose target files are byte-for-byte
identical to builds `24226502` and `24162678`; each new build must be
re-verified by extracting and hashing the stock asset, never assumed):

- stock UASSET SHA-256:
  `434f350c4d47ed7168747d66c68bda0a23c5fe94927267713d7d9b7d6aa39e56`
- stock UEXP SHA-256:
  `dd75dc715cd2dc71270485b82b9139066561500aaca32f884e75fd41022518b0`
- patched UEXP SHA-256:
  `283e9d3212961ec575cd15d869cf5af64cecdd13e3b230bd3b888cfc0da27a45`
- release PAK SHA-256:
  `1998125961ea66886ae41d71fe15ec2d555d045b980bc487ac5a6ea2a92d0c54`

The release PAK uses the game's `V12Outlast` PAK variant, mount point
`../../../`, and Zlib compression. It contains only `TrialBoardTab.uasset` and
`TrialBoardTab.uexp`. Use a legally obtained, compatible packer and set
`RAYON_NUM_THREADS=1` when using a Rayon-based repacker if deterministic entry
ordering matters. Always inspect the packed index and asset hashes afterward.

Do not redistribute extracted stock assets or encryption keys with the source
archive. Mods that replace the same `TrialBoardTab` asset are incompatible.
