# Runtime rating splits (GN7E69 revision 0)

NSRContent's application policy 4 separates normal-pass velocity (`PPVL`)
from native passing (`PPSS`), and movement acceleration (`PCEL`) from top
speed (`PSPD`). The launcher supplies a canonical, peer-agreed
`nsrb.runtime-content.v4` bundle before boot. Its object has the v1 ratings
shape and may include the v2 `draft` member and v3 `playbooks` member. The complete bundle SHA-256,
release version and policy participate in the existing M2 agreement. Older
forks reject the format. This is a compiled allowlist, not executable content.

## Neutral behavior and delivery

No split entry means no split consumer hook. An unset/zero PPVL inherits
the live native passing cache, including modifiers. Stock PCEL metadata is
inert: only an explicit nonzero v4 PCEL entry opts a named player into the
acceleration consumer. Zero/unset uses live PSPD-based acceleration. Existing
v1/v2/v3 PCEL packs retain their original native metadata semantics.

PCEL activation is meaningful even when the requested value already equals
the packed original. Policy 4 permits this semantic edit; other no-op edits
remain invalid. Both split fields accept 0–99. Explicit values use integer
`value * 255 / 99` scaling. PPVL preserves the native velocity curve, which
already saturates around rating 30. Explicit split values are independent
of native passing/speed modifiers; broader equipment/gamebreaker balancing
requires separate qualification.

The editor exposes CSV fields and a web acceleration activation checkbox.
Publishing uses the runtime split recipe and retains the base image. The
launcher applies the agreed bundle at the native DB boundary. Acceleration
packs cannot be baked into an image by the editor. The old local PPVL image
compiler remains a compatibility tool, with its historical 320-name limit;
the runtime lookup has no such limit.

## Deterministic implementation

`NSRContentRead.h` plans all packed writes atomically. PPVL uses PLAY bits
790–796 only after checking 100-byte records, four audited anchors and
descriptor disjointness. The loader validates original instruction words and
verifies the installed HLE registrations before writing guest data. An
incomplete incremental build fails explicitly. Content cannot specify
addresses or instructions.

The immutable override map uses bounded, normalized ASCII first/last names.
Each hook reads the current restored player object; no mutable player-pointer
cache survives rollback. Live and resimulated calls execute the same lookup.
The hooks replace exactly these signed halfword loads into r0:

| Consumer | Address | Input | Continuation |
| --- | --- | --- | --- |
| Normal-pass velocity | `0x80103544` | player r31, native `+0xBC4` | `0x80103548` |
| Acceleration branch 1 | `0x8016D1B8` | player r30, native `+0xBC0` | `0x8016D1BC` |
| Acceleration branch 2 | `0x8016D26C` | player r30, native `+0xBC0` | `0x8016D270` |

PCEL reads live metadata at player `+0xB66`. The top-speed consumer at
`0x8016D12C` remains native. Other registers, CR and floating-point state are
preserved. Fixed continuations are essential: mid-block HLE dispatch need
not update ppc.pc to the displaced instruction.

`NSR_RB_COST=1` optionally reports lookup calls/nanoseconds at reset. This
host-only measurement never influences guest values and excludes full HLE
dispatch overhead. Private qualification instrumentation observes raw motion
at `0x800B26E0` and selected pass values at `0x80103548`. That diagnostic
patch is kept outside this repository; the production netplay source is
unchanged by this feature.

## Qualification boundary

Socket-free synthetic tests cover atomic virtual padding, malformed layout,
name lookup, live metadata, zero inheritance and invalid pointers. Release
qualification also needs fresh-reference RNG/whole-RAM forced rollback,
two-peer proxy play with both fields, neutral/default comparisons, a matched
sprint from rest and a matched normal throw. Preserve predicted-live dump
warnings; compare finalized snapshots only with identical engine-declared
exclusion masks. Never widen a mask to obtain a passing comparison.

This work does not establish other throw families, campaigns, Windows or
real two-PC qualification. No game image, roster records or dumps belong in
this repository. Revisit broader balancing after those cases have matched
controls, rather than changing every native rating consumer at once.

Linux qualification on 2026-10-24: stock and full-roster both-split forced
rollback pass RNG/whole-RAM/ARAM checks; both two-human proxy runs pass with
zero differing game bytes at the requested live checkpoints. Quick regression
passes 7 cases (2 expected skips), 445 Python tests pass, and the strict C++20
socket-free suite passes. A matched normal throw changes velocity while
preserving its target/error. An isolated native-motion control holds PSPD,
rest state and native cap fixed: PCEL 1/99 reaches 95% of the same cap in
46/23 native steps. This controlled function-level experiment is separate
from the unmodified-image gameplay gates and broader AI/platform testing.
