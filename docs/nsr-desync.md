# Stream diagnostic desync investigation

This records a reproducible correctness blocker without changing the digest or guest.

## 2026-10-30: desync investigation — stream diagnostic record, no repair

Task bases: NFLStreet `9e54352`, fork `58be5b4f99`; later mainline changes
from other lanes are outside this investigation.

GN7E69 revision 0, Reborn 2026.09.12, rollback image fingerprint
`B57A-8270-68C2`. The fixed seed `0x26dabb33`, automatic delay 3,
40 ms / 8 ms jitter / 2% loss / 2% reorder, proxy seed `0x4E5352`
reproduces the eight differing bytes at `0x802C8A24..0x802C8A2C`.
A replay on the retained exact-mainline control first reports frame 2344;
read/write instrumentation reports frame 2343. Timing shifts the first
reported frame without eliminating the original failure.

The boot-time I/O-root hypothesis is not supported by the writer. A static
scan across **all DOL sections**, including executable code in `data2`, finds
`0x801CB4EC` branching to `0x802C8780`, a stream-read diagnostic trampoline.
It records arguments in a ring at `0x802C8860`, then, when the destination
interval crosses `0x811A9B54`, copies five words to `0x802C8A20`:
handle, destination, file offset, length, and the context from `0x803EBC3C`.
The disputed stores are `0x802C87FC` (destination) and `0x802C8800`
(file offset). The trampoline replays the displaced prologue and returns to
`0x801CB4F0`; the native read path consumes the original arguments, not these
copies. Both the original verified Reborn source and rb3 contain this code;
it was not introduced by this investigation or the owned-image integration.

The existing read/write watch catches these stores at live/cursor 2343,
LR `0x801CA5A4`, audio thread `0x803AA828`. No read of either disputed word
appears in that instrumented match interval. There **is** an indirect reader
of the preceding handle: main thread `0x803A3F30`, PC `0x802CB078`, LR
`0x802CB014`, scans MEM1 in 0x20-byte steps for marker `0x544D646C`.
The recorded handle `0x1F` fails that comparison, so the conditional next
load at `0x802CB084` does not read `0x802C8A24` in the observed case.
Do not describe this record as wholly unread, or a direct-reference scan as
proof that no indirect gameplay reader exists.

The task-base both-peer watch supplies the causal timing control: peer 1
copies destination/file offset `0x811A5540/0x0CE86B80` at frame 2343, while
peer 0 copies the same pair at 2348. Their next identical pair,
`0x811A5480/0x0CF4EB80`, is copied at frames 4218 and 4223 respectively.
All four updates run on audio thread `0x803AA828`, with the same writer PCs
and LR. Thus a frame can compare two different last-read diagnostic records,
then a later checkpoint agrees again after both streams update the record.
Neither disputed word has a guest read in this watched match on either peer.
This explains the narrow transient mismatch; it does not prove every possible
reader in other lifecycle paths is harmless.

`IO_ROOT_WORDS` drives **enclosing heap-block** classification through
`IoObjectBlocks`; it does not exclude the static root words themselves.
The destination already lies in a peer-agreed audio allocation and the next
word is a file offset that fails the MEM1 pointer check. Adding these to the
root table would not remove the static-record mismatch.
No root, checksum mask, fixed-address window, guest-state patch, protocol or
simulation change was made. **Digest range version remains 11.**

The evidence supports asynchronous stream-read diagnostic copies as the
immediate source of the mismatch. Ownership closure remains incomplete:
the memory watch starts at rollback entry, and linear static propagation
cannot rule out every indirect reader in untested boot/mode/lifecycle paths.
The blocker stays open. Closing it requires boot-through-lifecycle read/write
coverage of both words, an audit of every identified indirect consumer and
any alias/escaping address, then a separately reviewed repair consistent with
the diagnostic ownership evidence. A passing other-seed matrix cannot close
the fixed-seed failure. Do not widen the adjacent exclusion or label these
words boot-time I/O roots as a substitute for that work.

Exact commands, source/image identities, watches, paired snapshots and
qualification results live in the private `local/work/codex/desync/REPORT.md`.
