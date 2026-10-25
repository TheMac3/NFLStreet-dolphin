# NFLStreet-dolphin (NFLStreetRollback)

Private fork of [Dolphin](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later)
for **NFLStreetRollback** (short form NSRB), the NFLStreet POC: Slippi-style netplay for NFL Street 2 (GN7E69) as
patched by the NFL Street Reborn mod. NFLStreetRollback uses a player's own
Reborn install; it is not part of Reborn. Internal names (`NSR*` sources,
`NSR_*` variables, `Tools/NSR`, `nsr-*` files) keep the older prefix. Base: Dolphin tag `2512`. Work branch:
`nsr-netplay`.

Changes from upstream:

- Passive per-game stats/MVP and snap/play-call export to the Dolphin user `GameStats/` folder;
  see [capture format and evidence](docs/nsr-game-stats.md).

- `Source/Core/Core/NSR/NSRNetplay.{h,cpp}`: HLE hook for the NFLStreet
  in-game netplay stub (milestone 1: delay-based lockstep over ENet, 2-4
  players, star topology with host relay, room-code hole punching through the
  NFLStreet rendezvous server when `NSR_RENDEZVOUS` is set).
- `Source/Core/Core/HLE/HLE.cpp`: registers the hook when `NSR_NETPLAY` is set.
- `Source/Core/Core/CMakeLists.txt`, `Source/Core/DolphinLib.props`: build lists.
- `.github/workflows/nsr-windows.yml`: the Windows x64 friends zip: Dolphin,
  the **NFLStreetRollback.exe** launcher and **NFLStreetRollback-Patcher.exe** (`Tools/NSR`, vendored from
  NFLStreet), README-FIRST.txt, VERSION.txt, SOURCE.txt and licences. The
  launcher plays rollback netplay only (stage 4); `NFLStreetRollback-debug.bat` is a
  direct-IP debug helper with the same rollback settings.

Design, the game-side patch, and test tooling live in the NFLStreet repo
(`docs/poc-milestone1.md`, `tools/isopatch.py`, `tools/detrun.py`).
Without `NSR_NETPLAY` set, the transport stays inactive. Rollback-patched
GN7E69 revision-0 images still enable fixed game hooks, including passive
results capture.

Room codes: set the repository Actions variable `NSR_RENDEZVOUS` to the room
server's `host:port` and CI ships it as `nsr-rendezvous.txt` in the download.

CI: `.github/workflows/nsr-windows.yml` builds Release x64 on every push to
`nsr-netplay` (or on demand: `gh workflow run nsr-windows.yml --ref <branch>`);
download the `NFLStreetRollback-windows-x64` artifact from the run. It
unzips to one `NFLStreetRollback/` folder (was `NFLStreet-rollback-windows-x64` /
`NFLStreet-Rollback/` before the rename).
