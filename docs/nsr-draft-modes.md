# Runtime Pickup draft policies

For NFL Street 2 GN7E69 disc 0 revision 0 / Reborn 2026.09.12, the launcher
selects alternating, snake or cocky (seven picks per team), with stock,
Team One or Team Two first. Native sides are independent of host/pad identity.
The owning NFLStreet repository's `docs/draftmodes.md` documents evidence,
selection, image compatibility and qualification; ship both branches together.

The injection implementation's `NSR_CONTENT_PACK` / full SHA mechanism accepts
strict `nsrb.runtime-content.v2`: v1 format/version/packs plus exactly
`draft: {order, first, policy: 1}`. Full combined SHA, version and application
policy 2 use existing M2 fields. Enum values select fixed authored recipes;
no instructions or arbitrary write addresses come from a bundle. Default
ratings-only content retains v1. Launcher room admission also agrees the
selected policy and blocks changes during a room/game/update.

`NSRDraft.h` plans every original/cave word before any ratings/code writes.
Native STAT/template.dat registration completion at `0x8020D334` applies the
combined plan outside re-simulation, then safely resets instruction/JIT caches.
The native accepted-pick advance at `0x8001ABAC` is redirected only for Pickup,
two drafters and seven-player teams. Finalization stays native. Initialization's
query at `0x80018FC0` supplies the optional explicit first team. Authored code
uses zero-guarded dormant profiling caves `0x80411600`/`0x80411700`; the supported
rollback image excludes profiling/stress. All addresses are for the revision
above. Changed instructions, occupied caves or a wrong revision fail closed.

The header is generated from NFLStreet `tools/draftmodes.py` by
`python3 tools/export_draft_recipe.py OWN_FORK`, run in that repository.
Re-run it after recipe changes; never replace authored arrays with extracted
code or distribute game data. `scripts/sync-launcher-to-fork.sh OWN_FORK`
vendors the launcher after the owning-repository commit.

Socket-free strict C++ tests exercise all nine choices on synthetic memory,
no writes while planning, exact reapplication and conflicting/revision guards.
The owning repository's paired scripts and `tools/draftcheck.py` observe all
fourteen accepted picks on both humans, then run ordinary proxied rollback,
RNG, ownership, RAM and ARAM checks. Run common gates 1 and 2 as well; native
counts alone do not establish a successful match.

Cocky qualification exposed a pre-existing dump-sidecar gap for the known
small audio-capable heap `0x804C4810`: when its last audio allocation was freed,
sidecars could omit its counters and free-space ranges. Sidecars now seed the
same validated descriptor as the digest and use the existing narrow rules.
Used game payloads stay checked; free ranges must be free in both dumps.
This diagnostic change adds no work to the live digest's heap-disabled call.

Windows/two-PC, CPU picks, cancel/re-enter, native reloads and natural-ending
multi-game behavior require release qualification. Planner reapplication tests
do not establish that every native reload boundary is supported.
