# Individual defender rush picker

`NSRAudibles.h` implements the opt-in GN7E69 revision-0 defender picker.
It replaces one selected native play sequence with terminal native message 2
(immediate rush), before the game's snap loader populates player AI queues.
The ordinary rollback image is sufficient. Do not combine it with the original
image-only audibles experiment or the optional trace cave.

The launcher exposes a default-off experimental setting, passed as
`NSR_AUDIBLES=1`. Every participant must opt in before boot. `Audibles.Policy`
in preboot agreement and the M2 session-rules fingerprint bind the policy and
version, including its captain and mirror rules. No content recipe is added.

Release buttons, press Z + D-pad Up together, release, then use Left/Right to
pick a defender. Up applies BLITZ; Down or Z cancels. Release before ordinary
controls resume. The picker consumes buttons, signed stick axes and analog
triggers through its entry/menu/release phases, retaining Start for pause.
Ordinary controls pass through while the picker is closed. Only defense in
native lineup flow 2 is eligible. The lowest mapped physical controller port
on that side is captain and may adjust any of its seven native slots. Partners
cannot write assignments. Registry-to-port mapping is resolved explicitly.

All mutable control state is in the trace-reserved guest MEM1 block
`0x80413A00..0x80413B00`, explicitly removed from the trace live-keep range when enabled, so ordinary
snapshots and state digests cover it. The inner simulation tick reapplies a
guest-owned consumption mask without advancing the picker a second time.
The frame filter runs after pad assembly and snapshot capture, on both live
and re-simulated frames. Never call it from a VI poll, renderer or local input
callback. No host clock, local player ID, separate command packet or host latch
participates in decisions. The native selected-play cache is still reloaded
by whole-play audibles/new plays. Flow and play/formation changes close stale
menus with the release barrier retained.

`CaptureAudibles` reads native roster names and guest picker state on the CPU
thread. Immutable text travels with the XFB, then ImGui draws it on screen and
into private capture textures. Drawing never mutates simulation. The HUD names
the defender, slot and Native play/BLITZ assignment, and shows release, captain
or unavailable feedback. Keep capture compositing enabled for audible text even
when the play-call privacy cover is absent.

Mirrored formations remain explicitly unsupported. The native snap loader's
formation/play capability gates, category-specific inversion and formation-slot
permutation require runtime consumption qualification together. The picker
rejects nonzero formation flip bytes and reports that boundary on screen.
Native special override queues and inversion cases are also unavailable.
Do not substitute a reversed slot index or assume formation index equals the
physical player. The original research and detailed address/evidence notes
live in NFLStreet's `docs/audibles.md`.

The socket-free production-header tests are in
`Tools/NSR/NSRRollbackNetplayTest.cpp`. They cover all seven slots on both sides,
mirror rejection, nonidentity port maps, ownership, release barriers and
byte-identical restore/replay. Runtime proxy, AI-consumption and visual evidence
are maintained in the owning NFLStreet repository, with game data kept private.

Next work: qualify native man/zone/QB-spy operands and targets, then extend this
picker. Receiver routes, Max Protect precedence, freehand routes and content
recipes are separate tasks. Keep the launcher preference off by default until
those defensive options are qualified.
