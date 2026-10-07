# NFLStreet-dolphin

Private fork of [Dolphin](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later)
for the NFLStreet POC: Slippi-style netplay for NFL Street 2 (GN7E69) as
patched by the NFL Street Reborn mod. Base: Dolphin tag `2512`. Work branch:
`nsr-netplay`.

Changes from upstream:

- `Source/Core/Core/NSR/NSRNetplay.{h,cpp}`: HLE hook for the NFLStreet
  in-game netplay stub (milestone 1: delay-based lockstep over ENet).
- `Source/Core/Core/HLE/HLE.cpp`: registers the hook when `NSR_NETPLAY` is set.
- `Source/Core/Core/CMakeLists.txt`, `Source/Core/DolphinLib.props`: build lists.
- `.github/workflows/nsr-windows.yml`: Windows x64 build artifact.

Design, the game-side patch, and test tooling live in the NFLStreet repo
(`docs/poc-milestone1.md`, `tools/isopatch.py`, `tools/detrun.py`).
Without `NSR_NETPLAY` set, this build behaves like stock Dolphin.

CI: `.github/workflows/nsr-windows.yml` builds Release x64 on every push to
`nsr-netplay`; download the `NFLStreet-dolphin-windows-x64` artifact from the run.
