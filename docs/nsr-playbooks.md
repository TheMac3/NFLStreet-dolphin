# Personal existing-play books

The launcher accepts signed per-user playbook revisions and builds one immutable
`nsrb.runtime-content.v3` bundle for the entire room. This fork validates that
bundle's expected SHA-256, bounds and native references. Both preboot agreement
and M2 compare the full combined digest and application policy 3. The launcher
requires `--nsr-runtime-content=2`; earlier v1/v2 content remains supported.

The v3 payload retains `version`, ratings `packs`, and `draft` from runtime v2,
adding `playbooks: {policy:1,captain:"lowest-pad",users:[{pad,pack}]}`. The user
list is contiguous pad order, including `pack:null` for stock. A custom pack has
`format:nsrb.playbook.v1`, opaque `owner`, `revision`, user-authored `name`, exact
`base_sha256`/`igdata_sha256`, and category groups `{entry,pbst,library,slots}`.
Only native references are supplied; packets cannot contain guest instructions,
raw memory writes, route commands or play art. Signature and image/library
checks belong to `Tools/NSR/nsr_playbooks.py` before Dolphin starts. The native
adapter independently checks the live category and allocated destination.

For GN7E69 disc 0 revision 0, HLE replaces the original `mr r3,r25` at
`0x80057D8C`, guarded by `0x7F23CB78` and the existing rollback-image guard.
At this native order-builder boundary r25 is team, r27 book tag, r26/r28 the
allocated order array/count and sp+0xF1C the PBST ID. It maps selected PBPL IDs
to native `ord_` indices, fills only the allocated byte array, sets selected
count r29 and resumes the game's count/unused-tail code at `0x80057EE4`.
Stock paths execute the displaced instruction and continue at `0x80057D90`.
No DB records, DOL words or query caches change. The menu array is normal
rollback-owned guest memory. Unsupported/re-simulated setup fails closed.

The existing native registry at `*(u32*)0x803EAB60` and pad map at `0x802EACAC`
resolve pad ownership. The lowest participating pad on a shared side is captain,
even if it chose stock; CPU-only sides retain native books. Binding is recomputed
on menu construction after a rematch, independently of the local peer identity.
No HTTP, mutable account lookup or additional per-frame host operation runs on
the simulation path. Book names are launcher labels, supporting 64 Unicode
characters (up to 256 UTF-8 bytes); they are not inserted into game text tables.

Socket-free synthetic selection/capacity/stale-schema/captain checks live in
`Tools/NSR/NSRRollbackNetplayTest.cpp`. Common deterministic gates, fresh-reference
custom-content gates, two-user rendered proxy, controlled rematch/Pickup, partial
slots and before-boot mismatch rejection were exercised privately on Linux.
Windows/separate-PC, campaign/profile variations and a natural-ending multi-game
soak remain separate release checks. Arbitrary routes, blocking/coverage and new
IDs are disabled pending command/art/dependency/budget qualification.

Launcher source remains owned by NFLStreet. Its `scripts/sync-launcher-to-fork.sh`
updates the complete vendored module/asset set; do not edit those copies here.
The Access editor, signer and account DB are server-side components and are not
bundled into this fork. No game data or production credentials belong in Git.
