#!/usr/bin/env python3
"""isopatch.py: patch main.dol code inside a GameCube ISO copy, in place.

Builds the test images for the determinism harness (backlog step 6) without
rebuilding the disc: each patch writes 32-bit words at a virtual address,
mapped to its file offset through main.dol's section table. Every patch
checks the original words first, so running it against the wrong image
fails loudly instead of corrupting it.

Patches (GN7E69 rev 0, Reborn 3.0 main.dol `6dccf385…`):

  freeze  Retrace-counted freeze for frame-exact RAM snapshots. Redirects the
          first call in the VI post-retrace dispatcher (fn_801CE260,
          `bl 0x801F7B04` at 0x801CE26C) to a routine in unused space at the
          end of Reborn's text section (0x80411800). The routine increments
          a counter at 0x80411F00, and when it equals the target N at
          0x80411F04 (N = 0: never) it disables interrupts and spins, so RAM
          stops changing. Otherwise it tail-jumps to the original call.
          The harness sets N at runtime through Dolphin's RAM file.
  seed    Make fn_801CF7AC (the shared OSGetTick RNG seed source) deterministic:
          it returns the seed word at 0x804114F4 (initially 0x13579BDF) and
          then advances it by 0x9E3779B9, so each RNG seeded in a session gets
          a different but reproducible value. NFLStreet-dolphin overwrites the
          word with a per-session seed agreed in the lobby (NSR_SEED), so every
          match differs while all machines stay identical.
  input   Scripted controller input (and the shape of the netplay input
          hook). Replaces Reborn's `bl 0x80411400` (remap + PADClamp) in the
          pad poll at 0x801E2730 with a routine at 0x80411880 that calls it,
          then, while a script entry is active, overwrites port 1's PADStatus
          in the poll buffer (0x803E92CC). Script at 0x80411A40: 8-byte
          entries (u32 until_retrace, u16 buttons, s8 stickX, s8 stickY),
          terminated by until = 0; the first entry with
          until > retrace counter applies. The error byte is set to 0 so the
          game sees a connected controller. The harness uploads the script at
          runtime, so an all-zero script means "no injection". Optional loop:
          words at 0x80411F08 (start S) and 0x80411F0C (length L); past S the
          script is read at S + (retrace - S) mod L (L = 0: no loop).
  seedlog Instrument the RNG seed source instead of fixing it: fn_801CF7AC's
          first word becomes a branch to 0x80411960, which reads the time
          base exactly as the original does (`mftb`) and appends
          (retrace, caller LR, seed) to a 14-entry log: count at 0x80411F10,
          16-byte entries from 0x80411F20. Answers "when is each RNG seeded,
          by whom, and with what". Mutually exclusive with `seed`.
  profile Time the in-game frame and the sim step in emulated ticks (time base,
          40.5 MHz on GameCube). Each of three calls is redirected to a
          wrapper in the free tail of Reborn's text (0x80411500..) that reads
          `mftb` around the original call and accumulates (total, count, max)
          at 0x804117C0 (InGame update, call at 0x800326D0), 0x804117D0
          (fn_800AD888, call at 0x80028654), 0x804117E0 (fn_8017CEDC, call at
          0x8002865C). Arguments (r3.., f1) and the r3 return pass through.
  stress  Rollback cost stress test: the sim-step call at 0x80028654 runs
          fn_800AD888 K times per frame (same f1 each time), K read from
          0x804117F0 (0 or 1 = normal). Game state is wrong afterwards; this
          only measures whether the frame budget (emulated and host) can absorb
          K-1 re-simulated frames. Mutually exclusive with `profile`.
  netplay POC milestone 1 (needs `input`, which it then bypasses at the poll):
          the pad-poll call goes to a routine at 0x804119B0 that first calls
          the HLE stub at 0x804119AC with r3 = pad buffer, r4 = retrace
          counter (exchange of *raw* pads), then Reborn's remap + PADClamp on
          the agreed pads. The script table is read by the HLE side. The stub is `blr`; NFLStreet-dolphin replaces
          it with NSRNetplay::Hook when NSR_NETPLAY is set (lockstep pad
          exchange). Under stock Dolphin the image plays normally.
  trace   Diagnostics for milestone 2: log entries to selected functions into
          a 128-entry ring at 0x804132C0 (count at +0; entries at +0x10, each
          (retrace << 8 | function id, time-base low)). Each function's first
          instruction (all are `stwu r1, -X(r1)`) becomes a branch to a
          trampoline after the ring that logs, runs the displaced
          instruction, and jumps back. Needs freeze (retrace counter).
          IDs: see TRACE_FUNCS.
  rollback POC milestone 2 (needs freeze; replaces input/netplay at the poll
          and profile/stress at the sim calls; needs NFLStreet-dolphin, see
          docs/poc-milestone2.md). Uses free data at 0x80416900..0x80416A80
          (zero in every dump, unreferenced). Four HLE stubs (0x80416900 poll,
          0x80416904 tick, 0x80416908 tick end, 0x8041690C time; plain
          `blr` here).
          Poll: the tail of fn_801E26FC (0x801E271C..0x801E2730) becomes one
          call to a routine that does what the tail did (clear, PADRead,
          Reborn's remap + PADClamp) on a scratch buffer (0x80416920) and
          calls the poll stub twice (r3 = scratch, r4 = retrace counter,
          r5 = 0 before the remap, 1 after). Dolphin copies the processed
          pads into the game's pad buffer, except during re-simulation.
          Tick: InGame's tick loop body (`sim1(n * k); sim2(n)` at
          0x80028654/0x8002865C) becomes a routine that loops: call the tick
          stub (r3 = pad buffer; Dolphin may restore an earlier state and
          write that tick's pads), run sim part 1 and part 2, call the
          tick-end stub (0x80416908), repeat while bit 1 of the stub's r3 is
          set.
          Frame: the only call of InGame::fn_8002854C (0x800326D0) goes to
          a frame routine (0x80416A80) that loops frame stub (0x80416914),
          InGame, frame-end stub (0x80416918) while Dolphin asks, so a
          rollback re-runs whole frames (Slippi's model); Dolphin skips the
          present on re-simulated frames and fixes one tick per frame.
          Time: the direct `mftb r3` in game code at 0x80198924 calls the
          time stub (0x8041690C) instead, so Dolphin can return tick-based
          time during ticks (re-simulation must see the same time).

  draft-snake / draft-cocky  Pickup-only accepted-pick order; requires rollback.
          Optional draft-first-team-one / draft-first-team-two selects the
          native first side. No draft flags means stock alternating/order.
          See tools/draftmodes.py and docs/draftmodes.md.

Usage: tools/isopatch.py SRC.iso DST.iso freeze [seed | seedlog] [input [netplay] | rollback] [profile | stress] [trace]
"""
from __future__ import annotations

import os
import struct
import sys

FREEZE_CODE = 0x80411800
INPUT_CODE = 0x80411880
INPUT_SCRIPT = 0x80411A40   # up to 0x80411F00: 152 entries
POLL_CALL = 0x801E2730
REBORN_REMAP = 0x80411400
PAD_BUF = 0x803E92CC
FREEZE_COUNTER = 0x80411F00  # +4 = target N
DISPATCH_CALL = 0x801CE26C
ORIGINAL_CALLEE = 0x801F7B04
SEED_FN = 0x801CF7AC
SEED = 0x13579BDF            # default seed word value (offline/harness runs)
SEED_WORD = 0x804114F4       # must match NSRNetplay::SEED_ADDRESS
SEED_STEP = 0x9E3779B9       # Weyl increment between successive seeds
SEEDLOG_CODE = 0x80411960    # after the input routine
NETPLAY_STUB = 0x804119AC     # must match NSRNetplay::STUB_ADDRESS
NETPLAY_CODE = 0x804119B0
INPUT_LOOP = 0x80411F08      # script loop start S, +4 = loop length L (0 = no loop)
SEEDLOG = 0x80411F10         # count; entries at +0x10, 16 bytes each
SEEDLOG_MAX = 14
PROFILE_CODE = 0x80411500
PROFILE_ACC = 0x804117C0
STRESS_K = 0x804117F0
TRACE_RING = 0x804132C0       # zero in every dump; Reborn doesn't use it
TRACE_ENTRIES = 128
TRACE_CODE = TRACE_RING + 0x10 + TRACE_ENTRIES * 8   # 0x804136D0, room to 0x80413E00
TRACE_FUNCS = [  # (id, address, label)
    (1, 0x801E26FC, "pad poll (VI interrupt)"),
    (2, 0x801E1E50, "input consumer (descriptor update)"),
    (3, 0x8002854C, "InGame frame"),
    (4, 0x800AD888, "sim step part 1"),
    (5, 0x8017CEDC, "sim step part 2"),
    (6, 0x80028140, "InGame input handler"),
    (7, 0x801C601C, "input flush A"),
    (8, 0x801C60BC, "input flush B"),
    (9, 0x801C6280, "input handler register"),
]
RB_POLL_STUB = 0x80416900    # must match NSRNetplay::RB_POLL_STUB
RB_TICK_STUB = 0x80416904    # must match NSRNetplay::RB_TICK_STUB
RB_TICK_END_STUB = 0x80416908  # must match NSRNetplay::RB_TICK_END_STUB
RB_TIME_STUB = 0x8041690C    # must match NSRNetplay::RB_TIME_STUB
RB_GATE_RETURN = 0x80416910  # HLE only (NSRNetplay::RB_GATE_RETURN)
RB_FRAME_STUB = 0x80416914      # must match NSRNetplay::RB_FRAME_STUB
RB_FRAME_END_STUB = 0x80416918  # must match NSRNetplay::RB_FRAME_END_STUB
RB_FRAME_CODE = 0x80416A80      # frame routine (after the tick routine)
# NFLStreet-dolphin's audio clock keeps its model state (speech queues, virtual
# sound handles, bank-upload waits) in this free data (zero in the image,
# restored by rollback with the game). Patch code must stay below it.
AUDIO_CLOCK_STATE = 0x80416B00  # must match NSRNetplay AC_BASE (..0x80417000)
# The rollback unit is one main-loop iteration, fn_80026AD0 -> fn_801CBFA8:
# it first dispatches queued messages (the whole draw traversal, UI and
# camera updates run from there), then updates the active groups (InGame
# Group -> InGame::fn_8002854C, the sim). Wrapping only InGame left the draw
# pass's own state (camera smoothing, view matrices) k frames behind after
# every rollback, which drifted until gameplay diverged.
MAIN_ITER = 0x80026AD0          # one main-loop iteration (no args, void)
MAIN_ITER_SITE = 0x80026B64     # its only call, in the main loop fn_80026AF8
# The game's VI pre-retrace callback (retrace counter at *(0x803EBE44)+0x20
# and a retrace-driven real-time clock). During the rollback session Dolphin
# lets only the frame routine's call run (once per frame, live or
# re-simulated), not the interrupt's, so both counters follow frames, not
# retraces (a frame can take more than one retrace; peers differ).
VI_PRE_CB = 0x801F7DF4
# Game code that reads the time base directly (`mftb r3` in fn_801985FC,
# which saves LR, so a `bl` there is safe). Re-simulation needs the same value
# a tick saw the first time, so it goes through Dolphin.
MFTB_SITES = [0x80198924]
MFTB_R3 = 0x7C6C42E6
RB_SCRATCH = 0x80416920      # PADStatus[4] that PADRead fills in a rollback image
RB_POLL_CODE = 0x80416960
RB_TICK_CODE = 0x80416A10
RB_END = 0x80416A80          # must match NSRNetplay RB_KEEP
POLL_TAIL = 0x801E271C       # addi r3,r30,buf; bl clear; addi; bl PADRead; addi; bl remap
POLL_CLEAR = 0x801D34D0
PAD_READ = 0x8024734C
SIM1, SIM2 = 0x800AD888, 0x8017CEDC
SIM1_SITE, SIM2_SITE = 0x80028654, 0x8002865C
TICK_SCALE_SDA = -0x7FDC     # lfs f1, -0x7fdc(r13): the k in sim1(n * k)
PROFILE_SITES = [  # (call site, original callee) -> accumulator PROFILE_ACC + 16*i
    (0x800326D0, 0x8002854C),  # InGame::fn_8002854C (whole in-game frame)
    (0x80028654, 0x800AD888),  # sim step part 1
    (0x8002865C, 0x8017CEDC),  # sim step part 2
]


# --- tiny PowerPC encoder (only what these patches need) -------------------
def b(src: int, dst: int, link: bool = False) -> int:
    off = dst - src
    assert -0x2000000 <= off < 0x2000000 and off % 4 == 0
    return (18 << 26) | (off & 0x03FFFFFC) | int(link)


def bne(src: int, dst: int) -> int:  # bc 4, cr0.eq
    off = dst - src
    assert -0x8000 <= off < 0x8000 and off % 4 == 0
    return (16 << 26) | (4 << 21) | (2 << 16) | (off & 0xFFFC)


def lis(rd, imm): return (15 << 26) | (rd << 21) | (imm & 0xFFFF)
def ori(ra, rs, imm): return (24 << 26) | (rs << 21) | (ra << 16) | (imm & 0xFFFF)
def addi(rd, ra, imm): return (14 << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)
def lwz(rd, d, ra): return (32 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)
def stw(rs, d, ra): return (36 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)
def cmpw(ra, rb): return (31 << 26) | (ra << 16) | (rb << 11) | (0 << 1)
def mfmsr(rd): return (31 << 26) | (rd << 21) | (83 << 1)
def mtmsr(rs): return (31 << 26) | (rs << 21) | (146 << 1)
def isync(): return 0x4C00012C
def blr(): return 0x4E800020
def stwu(rs, d, ra): return (37 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)
def lhz(rd, d, ra): return (40 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)
def lbz(rd, d, ra): return (34 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)
def sth(rs, d, ra): return (44 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)
def stb(rs, d, ra): return (38 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)
def mflr(rd): return (31 << 26) | (rd << 21) | (8 << 16) | (339 << 1)
def mtlr(rs): return (31 << 26) | (rs << 21) | (8 << 16) | (467 << 1)
def mr(ra, rs): return (31 << 26) | (rs << 21) | (ra << 16) | (rs << 11) | (444 << 1)
def cmpwi(ra, imm): return (11 << 26) | (ra << 16) | (imm & 0xFFFF)
def cmplw(ra, rb): return (31 << 26) | (ra << 16) | (rb << 11) | (32 << 1)


def stfd(fs, d, ra): return (54 << 26) | (fs << 21) | (ra << 16) | (d & 0xFFFF)
def lfd(fd, d, ra): return (50 << 26) | (fd << 21) | (ra << 16) | (d & 0xFFFF)
def addic_(rd, ra, imm): return (13 << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)
def addis(rd, ra, imm): return (15 << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)
def subf(rd, ra, rb): return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (40 << 1)
def divwu(rd, ra, rb): return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (459 << 1)
def mullw(rd, ra, rb): return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (235 << 1)
def mftb(rd): return (31 << 26) | (rd << 21) | (268 & 0x1F) << 16 | (268 >> 5) << 11 | (371 << 1)
def cmplwi(ra, imm): return (10 << 26) | (ra << 16) | (imm & 0xFFFF)
def slwi(ra, rs, n): return rlwinm(ra, rs, n, 0, 31 - n)
def add(rd, ra, rb): return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266 << 1)


def bc(src: int, dst: int, bo: int, bi: int) -> int:
    off = dst - src
    assert -0x8000 <= off < 0x8000 and off % 4 == 0
    return (16 << 26) | (bo << 21) | (bi << 16) | (off & 0xFFFC)


def andi_(ra, rs, imm): return (28 << 26) | (rs << 21) | (ra << 16) | (imm & 0xFFFF)
def lfs(fd, d, ra): return (48 << 26) | (fd << 21) | (ra << 16) | (d & 0xFFFF)
def fmuls(fd, fa, fc): return (59 << 26) | (fd << 21) | (fa << 16) | (fc << 6) | (25 << 1)
def fmr(fd, fb): return (63 << 26) | (fd << 21) | (fb << 11) | (72 << 1)
def nop(): return 0x60000000
def rlwinm(ra, rs, sh, mb, me): return (21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1)


def seed_routine() -> list[int]:
    """fn_801CF7AC replacement: r3 = *SEED_WORD; *SEED_WORD += SEED_STEP."""
    hi, lo = SEED_WORD >> 16, SEED_WORD & 0xFFFF
    assert lo < 0x8000  # lwz/stw displacement is signed
    step_lo = SEED_STEP & 0xFFFF
    step_hi = (SEED_STEP >> 16) + (1 if step_lo & 0x8000 else 0)  # addi sign-extends
    # r11 as scratch, not r0: in addi/addis a source register of r0 means
    # the literal 0, so "addi r0, r0, x" would be "li r0, x".
    return [
        lis(12, hi), lwz(3, lo, 12),
        addis(11, 3, step_hi), addi(11, 11, step_lo),
        stw(11, lo, 12), blr(),
    ]


def freeze_routine() -> list[int]:
    a = FREEZE_CODE
    code = [
        lis(11, FREEZE_COUNTER >> 16),
        ori(11, 11, FREEZE_COUNTER & 0xFFFF),
        lwz(12, 0, 11),
        addi(12, 12, 1),
        stw(12, 0, 11),
        lwz(11, 4, 11),          # N
        cmpw(12, 11),
        None,                    # bne tail (filled below)
        mfmsr(11),
        rlwinm(11, 11, 0, 17, 15),  # clear MSR[EE]
        mtmsr(11),
        isync(),
        None,                    # spin: b spin
        None,                    # tail: b ORIGINAL_CALLEE
    ]
    spin = a + 12 * 4
    tail = a + 13 * 4
    code[7] = bne(a + 7 * 4, tail)
    code[12] = b(spin, spin)
    code[13] = b(tail, ORIGINAL_CALLEE)
    return code


def input_routine() -> list[int]:
    """Assemble the scripted-input routine with label fixups."""
    a = INPUT_CODE
    prog: list = [
        stwu(1, -0x10, 1), mflr(0), stw(0, 0x14, 1), stw(31, 0xC, 1),
        mr(31, 3),                                  # r31 = PADStatus[4] buffer
        ("bl", REBORN_REMAP),                       # Reborn remap -> PADClamp
        lis(11, FREEZE_COUNTER >> 16), ori(11, 11, FREEZE_COUNTER & 0xFFFF),
        lwz(12, 0, 11),                             # r12 = retrace counter
        # Optional loop: past S, use S + (counter - S) mod L.
        lis(9, INPUT_LOOP >> 16), ori(9, 9, INPUT_LOOP & 0xFFFF),
        lwz(8, 0, 9), lwz(7, 4, 9),
        cmpwi(7, 0), ("beq", "noloop"),
        cmplw(12, 8), ("blt", "noloop"),
        subf(6, 8, 12), divwu(5, 6, 7), mullw(5, 5, 7), subf(6, 5, 6), add(12, 8, 6),
        "noloop",
        lis(10, INPUT_SCRIPT >> 16), ori(10, 10, INPUT_SCRIPT & 0xFFFF),
        "loop",
        lwz(9, 0, 10),
        cmpwi(9, 0), ("beq", "done"),               # terminator: no injection
        cmplw(12, 9), ("blt", "found"),             # counter < until -> this entry
        addi(10, 10, 8), ("b", "loop"),
        "found",
        lhz(9, 4, 10), sth(9, 0, 31),               # buttons
        lbz(9, 6, 10), stb(9, 2, 31),               # stickX
        lbz(9, 7, 10), stb(9, 3, 31),               # stickY
        addi(9, 0, 0),                              # li r9, 0
        stb(9, 4, 31), stb(9, 5, 31), stb(9, 6, 31), stb(9, 7, 31),
        stb(9, 8, 31), stb(9, 9, 31), stb(9, 10, 31),  # sticks/triggers 0, err = 0
        "done",
        lwz(31, 0xC, 1), lwz(0, 0x14, 1), mtlr(0), addi(1, 1, 0x10), blr(),
    ]
    labels, addr = {}, a
    for item in prog:
        if isinstance(item, str):
            labels[item] = addr
        else:
            addr += 4
    out, addr = [], a
    for item in prog:
        if isinstance(item, str):
            continue
        if isinstance(item, tuple):
            op, tgt = item
            tgt = labels.get(tgt, tgt)
            if op == "bl":
                out.append(b(addr, tgt, link=True))
            elif op == "b":
                out.append(b(addr, tgt))
            elif op == "beq":
                out.append(bc(addr, tgt, 12, 2))
            elif op == "blt":
                out.append(bc(addr, tgt, 12, 0))
        else:
            out.append(item)
        addr += 4
    assert a + 4 * len(out) <= SEEDLOG_CODE
    return out


def seedlog_routine() -> list[int]:
    """mftb r3, then log (retrace, LR, r3) if the table has room; blr."""
    a = SEEDLOG_CODE
    code = [
        mftb(3),
        lis(11, SEEDLOG >> 16), ori(11, 11, SEEDLOG & 0xFFFF),
        lwz(12, 0, 11),
        cmplwi(12, SEEDLOG_MAX),
        None,                          # bge done
        slwi(10, 12, 4), add(10, 10, 11),
        lis(9, FREEZE_COUNTER >> 16), ori(9, 9, FREEZE_COUNTER & 0xFFFF), lwz(9, 0, 9),
        stw(9, 0x10, 10),              # retrace
        mflr(9), stw(9, 0x14, 10),     # caller
        stw(3, 0x18, 10),              # seed
        addi(12, 12, 1), stw(12, 0, 11),
        blr(),                         # done
    ]
    done = a + 4 * (len(code) - 1)
    code[5] = bc(a + 5 * 4, done, 4, 0)   # bge: branch if !lt
    assert a + 4 * len(code) <= INPUT_SCRIPT
    return code


def profile_wrapper(addr: int, callee: int, acc: int) -> list[int]:
    """Timing wrapper: mftb, call original, mftb, accumulate; returns r3."""
    code = [
        stwu(1, -0x20, 1), mflr(0), stw(0, 0x24, 1), stw(31, 0x1C, 1), stw(30, 0x18, 1),
        mftb(31),
        None,                                   # bl callee
        mftb(30),
        stw(3, 0x14, 1),                        # keep the callee's return value
        subf(30, 31, 30),                       # r30 = end - start
        lis(11, acc >> 16), ori(11, 11, acc & 0xFFFF),
        lwz(12, 0, 11), add(12, 12, 30), stw(12, 0, 11),   # total
        lwz(12, 4, 11), addi(12, 12, 1), stw(12, 4, 11),   # count
        lwz(12, 8, 11), cmplw(30, 12), None, stw(30, 8, 11),  # max
        lwz(3, 0x14, 1), lwz(30, 0x18, 1), lwz(31, 0x1C, 1),
        lwz(0, 0x24, 1), mtlr(0), addi(1, 1, 0x20), blr(),
    ]
    code[6] = b(addr + 6 * 4, callee, link=True)
    skip = 22                                   # index of `lwz r3, 0x14(r1)`
    code[20] = bc(addr + 20 * 4, addr + skip * 4, 4, 1)  # ble skip (branch if !gt)
    return code


def profile_routines() -> list[tuple[int, list[int]]]:
    out, addr = [], PROFILE_CODE
    for i, (_, callee) in enumerate(PROFILE_SITES):
        w = profile_wrapper(addr, callee, PROFILE_ACC + 16 * i)
        out.append((addr, w))
        addr += 4 * len(w)
    assert addr <= PROFILE_ACC
    return out


def stress_routine() -> list[int]:
    a = PROFILE_CODE
    site, callee = PROFILE_SITES[1]
    code = [
        stwu(1, -0x20, 1), mflr(0), stw(0, 0x24, 1), stw(31, 0x1C, 1), stfd(1, 0x10, 1),
        lis(11, STRESS_K >> 16), ori(11, 11, STRESS_K & 0xFFFF), lwz(31, 0, 11),
        cmpwi(31, 1), None, addi(31, 0, 1),     # if K < 1: K = 1
        lfd(1, 0x10, 1),                        # loop:
        None,                                   # bl callee
        addic_(31, 31, -1), None,               # bne loop
        lwz(31, 0x1C, 1), lwz(0, 0x24, 1), mtlr(0), addi(1, 1, 0x20), blr(),
    ]
    loop = a + 11 * 4
    code[9] = bc(a + 9 * 4, loop, 4, 0)         # bge loop (K >= 1)
    code[12] = b(a + 12 * 4, callee, link=True)
    code[14] = bc(a + 14 * 4, loop, 4, 2)       # bne loop
    return code


def netplay_routine() -> list[int]:
    """Pad poll -> HLE exchange on raw pads -> remap/PADClamp on agreed pads.

    Order matters for lockstep: any game code that runs on a machine's *local*
    input before the agreed pads are in place executes a different number of
    instructions on each machine, so emulated time drifts apart (measured:
    timestamps off by 1-16 ticks within ~10 s). So the exchange comes first,
    straight after PADRead, and Reborn's remap + PADClamp then run on the
    agreed buffer, which is identical on both sides. Test scripts are read
    by the HLE side (zero emulated cost), not by game code.
    """
    a = NETPLAY_CODE
    code = [
        stwu(1, -0x10, 1), mflr(0), stw(0, 0x14, 1), stw(31, 0xC, 1),
        mr(31, 3),
        lis(4, FREEZE_COUNTER >> 16), ori(4, 4, FREEZE_COUNTER & 0xFFFF), lwz(4, 0, 4),
        None,                                    # bl NETPLAY_STUB (HLE exchange)
        mr(3, 31),
        None,                                    # bl REBORN_REMAP (remap -> PADClamp)
        # Register hygiene: leave the poll with identical volatile registers
        # and cr0 on both machines (interrupts save them into RAM).
        addi(3, 0, 0), addi(4, 0, 0), addi(5, 0, 0), addi(6, 0, 0), addi(7, 0, 0),
        addi(8, 0, 0), addi(9, 0, 0), addi(10, 0, 0), addi(11, 0, 0), addi(12, 0, 0),
        cmpwi(3, 0),
        lwz(31, 0xC, 1), lwz(0, 0x14, 1), mtlr(0), addi(1, 1, 0x10), blr(),
    ]
    code[8] = b(a + 8 * 4, NETPLAY_STUB, link=True)
    code[10] = b(a + 10 * 4, REBORN_REMAP, link=True)
    assert a + 4 * len(code) <= INPUT_SCRIPT
    return code


def _load(reg: int, value: int) -> list[int]:
    return [lis(reg, value >> 16), ori(reg, reg, value & 0xFFFF)]


def rollback_poll_routine() -> list[int]:
    """Pad poll for rollback images: the original PADRead + remap, on scratch.

    Called from the poll with r4 = 0x30, r5 = 0, r6 = 1 already set up for
    the clear call (fn_801D34D0), exactly as the original tail had them.
    The poll stub runs twice: phase 0 on the raw pads (Dolphin may replace
    them: test scripts, later the netplay port layout), phase 1 on the
    processed pads (Dolphin records them and copies them to the game's pad
    buffer, except while it is re-simulating ticks). Reborn's remap still
    runs here, in the interrupt, as in the unpatched game.
    """
    a = RB_POLL_CODE
    stub_call = [*_load(3, RB_SCRATCH), *_load(4, FREEZE_COUNTER), lwz(4, 0, 4)]
    code = [stwu(1, -0x10, 1), mflr(0), stw(0, 0x14, 1),
            *_load(3, RB_SCRATCH), None,                 # bl clear(scratch, 0x30, 0, 1)
            *_load(3, RB_SCRATCH), None,                 # bl PADRead(scratch)
            *stub_call, addi(5, 0, 0), None,             # bl RB_POLL_STUB (phase 0)
            *_load(3, RB_SCRATCH), None,                 # bl REBORN_REMAP(scratch)
            *stub_call, addi(5, 0, 1), None]             # bl RB_POLL_STUB (phase 1)
    # Register hygiene, as in the netplay routine: the poll runs in an
    # interrupt, whose saved context lands in RAM.
    code += [addi(r, 0, 0) for r in range(3, 13)] + [cmpwi(3, 0)]
    code += [lwz(0, 0x14, 1), mtlr(0), addi(1, 1, 0x10), blr()]
    targets = iter([POLL_CLEAR, PAD_READ, RB_POLL_STUB, REBORN_REMAP, RB_POLL_STUB])
    for i, w in enumerate(code):
        if w is None:
            code[i] = b(a + 4 * i, next(targets), link=True)
    assert a + 4 * len(code) <= RB_TICK_CODE
    return code


def rollback_tick_routine() -> list[int]:
    """Replaces `bl sim1` in InGame's tick loop (f31 = n, f1 = n * k)."""
    a = RB_TICK_CODE
    code = [stwu(1, -0x20, 1), mflr(0), stw(0, 0x24, 1), stw(31, 0x1C, 1)]
    loop = len(code)
    code += [*_load(3, PAD_BUF), None,                                # bl RB_TICK_STUB
             mr(31, 3),
             lfs(1, TICK_SCALE_SDA, 13), fmuls(1, 31, 1), None,       # bl SIM1
             fmr(1, 31), None,                                        # bl SIM2
             None,                                                    # bl RB_TICK_END_STUB
             andi_(0, 31, 1), None]                                   # bne loop
    code += [lwz(31, 0x1C, 1), lwz(0, 0x24, 1), mtlr(0), addi(1, 1, 0x20), blr()]
    holes = [i for i, w in enumerate(code) if w is None]
    stub, sim1, sim2, end, bne_ = holes
    code[stub] = b(a + 4 * stub, RB_TICK_STUB, link=True)
    code[sim1] = b(a + 4 * sim1, SIM1, link=True)
    code[sim2] = b(a + 4 * sim2, SIM2, link=True)
    code[end] = b(a + 4 * end, RB_TICK_END_STUB, link=True)
    code[bne_] = bc(a + 4 * bne_, a + 4 * loop, 4, 2)
    assert a + 4 * len(code) <= RB_END
    return code


def rollback_frame_routine() -> list[int]:
    """Replaces `bl fn_80026AD0` (frame mode): run main-loop iterations while Dolphin asks.

    Loop: frame stub (r3 bit 1 = run another frame after this one), the VI
    pre-retrace callback (Dolphin runs it here only during the session), one
    main-loop iteration, frame-end stub.
    """
    a = RB_FRAME_CODE
    code = [stwu(1, -0x20, 1), mflr(0), stw(0, 0x24, 1), stw(31, 0x1C, 1)]
    loop = len(code)
    code += [None,                                   # bl RB_FRAME_STUB
             mr(31, 3), None,                        # bl VI_PRE_CB (gated by Dolphin)
             None,                                   # bl MAIN_ITER
             None,                                   # bl RB_FRAME_END_STUB
             andi_(0, 31, 1), None,                  # bne loop
             lwz(31, 0x1C, 1), lwz(0, 0x24, 1), mtlr(0), addi(1, 1, 0x20), blr()]
    holes = [i for i, w in enumerate(code) if w is None]
    stub, vicb, main, end, bne_ = holes
    code[stub] = b(a + 4 * stub, RB_FRAME_STUB, link=True)
    code[vicb] = b(a + 4 * vicb, VI_PRE_CB, link=True)
    code[main] = b(a + 4 * main, MAIN_ITER, link=True)
    code[end] = b(a + 4 * end, RB_FRAME_END_STUB, link=True)
    code[bne_] = bc(a + 4 * bne_, a + 4 * loop, 4, 2)
    assert a + 4 * len(code) <= AUDIO_CLOCK_STATE
    return code


class PatchError(Exception):
    """The image isn't the one these patches were written for (or bad options)."""


def trace_trampoline(addr: int, func: int, fid: int, displaced: int) -> list[int]:
    ring_hi, ring_lo = TRACE_RING >> 16, TRACE_RING & 0xFFFF
    ctr_hi, ctr_lo = FREEZE_COUNTER >> 16, FREEZE_COUNTER & 0xFFFF
    mask_mb = 32 - 3 - (TRACE_ENTRIES.bit_length() - 1)  # (count % N) * 8
    code = [
        stwu(1, -0x20, 1), stw(10, 8, 1), stw(11, 0xC, 1), stw(12, 0x10, 1),
        lis(11, ring_hi), ori(11, 11, ring_lo),
        lwz(12, 0, 11), addi(10, 12, 1), stw(10, 0, 11),
        rlwinm(12, 12, 3, mask_mb, 28), add(12, 12, 11),
        lis(10, ctr_hi), ori(10, 10, ctr_lo), lwz(10, 0, 10),
        slwi(10, 10, 8), ori(10, 10, fid), stw(10, 0x10, 12),
        mftb(10), stw(10, 0x14, 12),
        lwz(10, 8, 1), lwz(11, 0xC, 1), lwz(12, 0x10, 1), addi(1, 1, 0x20),
        displaced,
        None,
    ]
    code[-1] = b(addr + 4 * (len(code) - 1), func + 4)
    return code


# --- ISO / DOL mapping -------------------------------------------------------
def dol_va_to_iso(iso, va: int) -> int:
    iso.seek(0x420)
    dol_off = struct.unpack(">I", iso.read(4))[0]
    iso.seek(dol_off)
    hdr = iso.read(0x100)
    offs = struct.unpack_from(">18I", hdr, 0)
    addrs = struct.unpack_from(">18I", hdr, 0x48)
    sizes = struct.unpack_from(">18I", hdr, 0x90)
    for o, a, s in zip(offs, addrs, sizes):
        if s and a <= va < a + s:
            return dol_off + o + (va - a)
    raise ValueError(f"{va:#x} is not in any main.dol section")


def write_words(iso, va: int, words: list[int], expect: list[int] | None = None) -> None:
    off = dol_va_to_iso(iso, va)
    iso.seek(off)
    cur = list(struct.unpack(f">{len(words)}I", iso.read(4 * len(words))))
    if expect is not None and cur != expect:
        raise PatchError(f"unexpected original words at {va:#x}: "
                         f"{' '.join(f'{w:08x}' for w in cur)} (wrong image?)")
    iso.seek(off)
    iso.write(struct.pack(f">{len(words)}I", *words))


PATCHES = {"freeze", "seed", "seedlog", "input", "profile", "stress", "netplay", "trace",
           "rollback", "ratingsplit", "draft-snake", "draft-cocky", "draft-first-team-one", "draft-first-team-two"}
# Milestone-1 delay-based lockstep image (superseded for friends; kept so the
# lockstep path and its tests stay reproducible).
LOCKSTEP_SET = ("freeze", "seed", "input", "netplay")
# Stage-4 rollback image: lockstep menus, rollback matches. The fork needs
# NSR_RB_NET=1 with it; the milestone-1 lockstep hook is absent, so this image
# can't play lockstep netplay (the mode is a property of the image).
ROLLBACK_SET = ("freeze", "seed", "rollback")
NETPLAY_SET = ROLLBACK_SET  # what friends' netplay images use (nsr_patcher)

# Optional ratingsplit-v1: GN7E69 rev 0. PPSS keeps its native accuracy
# consumers. The normal throw velocity consumer reads PPVL independently.
# Match setup rekeys PGID, so use an exact first/last-name hash, collision-checked
# against every roster row by the builder. Reuse the audited trace cave; trace
# and ratingsplit are mutually exclusive. Reborn has live code after 0x80413E00.
RATINGSPLIT_SITE = 0x80103544  # lha r0,0xBC4(r31): normal throw speed penalty
RATINGSPLIT_CODE = 0x804132C0
RATINGSPLIT_TABLE = 0x804133C0
RATINGSPLIT_CAPACITY = 320
RATINGSPLIT_MAGIC = 0x52535631  # RSV1


def ratingsplit_routine() -> list[int]:
    """Read a separate velocity rating into r0, preserving all other registers.

    The displaced PPSS load remains the fallback when PPVL is zero/unset.
    Exact name bytes come from player+0xB6F / +0xB84; all inputs are image-owned
    or rollback-owned MEM1. The existing native speed formula remains intact.
    """
    code, labels, jumps = [], {}, []

    def emit(*words):
        """Append encoded words at the next cave address."""
        code.extend(words)

    def jump(label, condition=None):
        """Resolve a local branch after laying out the routine."""
        jumps.append((len(code), label, condition))
        emit(0)

    emit(stwu(1, -32, 1), stw(9, 8, 1),
         stw(10, 12, 1), stw(11, 16, 1), stw(12, 20, 1),
         0x7C000026, stw(0, 24, 1))
    emit(lis(9, 0x811C), ori(9, 9, 0x9DC5), lis(12, 0x0100), ori(12, 12, 0x0193))
    for index, offset in enumerate((0xB6F, 0xB84)):
        emit(addi(10, 31, offset))
        labels[f"hash{index}"] = len(code)
        emit(lbz(0, 0, 10), cmpwi(0, 0))
        jump(f"hashed{index}", (12, 2))
        emit((31 << 26) | (9 << 21) | (9 << 16) | (0 << 11) | (316 << 1),
             mullw(9, 9, 12), addi(10, 10, 1))  # xor r9,r9,r0
        jump(f"hash{index}")
        labels[f"hashed{index}"] = len(code)
        if index == 0:
            emit((26 << 26) | (9 << 21) | (9 << 16) | 255, mullw(9, 9, 12))
    emit(lis(10, RATINGSPLIT_TABLE >> 16), ori(10, 10, RATINGSPLIT_TABLE & 65535), lwz(11, 4, 10))
    labels["loop"] = len(code)
    emit(cmpwi(11, 0))
    jump("done", (12, 2))
    emit(lwz(12, 8, 10), cmpw(12, 9))
    jump("found", (12, 2))
    emit(addi(10, 10, 8), addi(11, 11, -1))
    jump("loop")
    labels["found"] = len(code)
    emit(lwz(12, 12, 10))
    jump("return")
    labels["done"] = len(code)
    emit((42 << 26) | (12 << 21) | (31 << 16) | 0xBC4)  # fallback lha r12,0xBC4(r31)
    labels["return"] = len(code)
    emit(lwz(0, 24, 1), 0x7C0FF120, mr(0, 12),
         lwz(9, 8, 1), lwz(10, 12, 1), lwz(11, 16, 1), lwz(12, 20, 1),
         addi(1, 1, 32))
    emit(b(RATINGSPLIT_CODE + len(code) * 4, RATINGSPLIT_SITE + 4))
    for i, label, condition in jumps:
        src, dst = RATINGSPLIT_CODE + 4 * i, RATINGSPLIT_CODE + 4 * labels[label]
        code[i] = bc(src, dst, *condition) if condition else b(src, dst)
    assert RATINGSPLIT_CODE + len(code) * 4 <= RATINGSPLIT_TABLE
    return code


def check_patch_set(patches: set[str]) -> None:
    """Reject unknown/conflicting patches before copying or mutating an image."""
    if not patches or not patches <= PATCHES:
        raise PatchError(f"unknown patches: {sorted(patches - PATCHES)}")
    # Draft choices are baked into the rollback image, never host-only runtime state.
    import draftmodes
    draftmodes.validate_patches(patches)
    for a, b_ in (("seed", "seedlog"), ("profile", "stress"), ("trace", "ratingsplit"), ("rollback", "input"),
                  ("rollback", "profile"), ("rollback", "stress")):
        if {a, b_} <= patches:
            raise PatchError(f"{a} and {b_} can't be combined")
    for need, by in (("freeze", "seedlog"), ("freeze", "input"), ("input", "netplay"),
                     ("freeze", "trace"), ("freeze", "rollback")):
        if by in patches and need not in patches:
            raise PatchError(f"{by} needs {need}")


def apply_patches(iso, patches: set[str]) -> None:
    """Patch an already-copied image file (opened r+b) in place."""
    check_patch_set(patches)
    if "ratingsplit" in patches:
        iso.seek(0)
        header = iso.read(8)
        if header[:6] != b"GN7E69" or header[7] != 0:
            raise PatchError("ratingsplit requires GN7E69 revision 0")
        words = ratingsplit_routine()
        write_words(iso, RATINGSPLIT_CODE, words, expect=[0] * len(words))
        table = [RATINGSPLIT_MAGIC, 0] + [0] * (2 * RATINGSPLIT_CAPACITY)
        write_words(iso, RATINGSPLIT_TABLE, table, expect=[0] * len(table))
        write_words(iso, RATINGSPLIT_SITE, [b(RATINGSPLIT_SITE, RATINGSPLIT_CODE)],
                    expect=[(42 << 26) | (31 << 16) | 0xBC4])
    if "freeze" in patches:
        routine = freeze_routine()
        write_words(iso, FREEZE_CODE, routine, expect=[0] * len(routine))
        write_words(iso, FREEZE_COUNTER, [0, 0], expect=[0, 0])
        write_words(iso, DISPATCH_CALL, [b(DISPATCH_CALL, FREEZE_CODE, link=True)],
                    expect=[b(DISPATCH_CALL, ORIGINAL_CALLEE, link=True)])
    if "seed" in patches:
        write_words(iso, SEED_FN, seed_routine(),
                    expect=[0x9421FFF8, 0x7C0802A6, 0x9001000C, 0x4806EFF5, 0x8001000C, 0x7C0803A6])
        write_words(iso, SEED_WORD, [SEED], expect=[0])
    if "seedlog" in patches:
        routine = seedlog_routine()
        write_words(iso, SEEDLOG_CODE, routine, expect=[0] * len(routine))
        write_words(iso, SEED_FN, [b(SEED_FN, SEEDLOG_CODE)], expect=[0x9421FFF8])
    if "profile" in patches:
        for (site, callee), (addr, words) in zip(PROFILE_SITES, profile_routines()):
            write_words(iso, addr, words, expect=[0] * len(words))
            write_words(iso, site, [b(site, addr, link=True)], expect=[b(site, callee, link=True)])
        write_words(iso, PROFILE_ACC, [0] * 12, expect=[0] * 12)
    if "stress" in patches:
        words = stress_routine()
        site, callee = PROFILE_SITES[1]
        write_words(iso, PROFILE_CODE, words, expect=[0] * len(words))
        write_words(iso, site, [b(site, PROFILE_CODE, link=True)], expect=[b(site, callee, link=True)])
    if "input" in patches:
        routine = input_routine()
        write_words(iso, INPUT_CODE, routine, expect=[0] * len(routine))
        write_words(iso, POLL_CALL, [b(POLL_CALL, INPUT_CODE, link=True)],
                    expect=[b(POLL_CALL, REBORN_REMAP, link=True)])
    if "trace" in patches:
        addr = TRACE_CODE
        write_words(iso, TRACE_RING, [0] * 4, expect=[0] * 4)
        for fid, func, _ in TRACE_FUNCS:
            iso.seek(dol_va_to_iso(iso, func))
            first = struct.unpack(">I", iso.read(4))[0]
            if first >> 16 != 0x9421:
                raise PatchError(f"trace: {func:#x} doesn't start with stwu r1 ({first:08x})")
            words = trace_trampoline(addr, func, fid, first)
            write_words(iso, addr, words, expect=[0] * len(words))
            write_words(iso, func, [b(func, addr)], expect=[first])
            addr += 4 * len(words)
        assert addr <= 0x80413E00, hex(addr)
    if "rollback" in patches:
        write_words(iso, RB_POLL_STUB, [blr()] * 7, expect=[0] * 7)
        words = rollback_frame_routine()
        write_words(iso, RB_FRAME_CODE, words, expect=[0] * len(words))
        write_words(iso, MAIN_ITER_SITE, [b(MAIN_ITER_SITE, RB_FRAME_CODE, link=True)],
                    expect=[b(MAIN_ITER_SITE, MAIN_ITER, link=True)])
        for site in MFTB_SITES:
            write_words(iso, site, [b(site, RB_TIME_STUB, link=True)], expect=[MFTB_R3])
        for addr, words in ((RB_POLL_CODE, rollback_poll_routine()),
                            (RB_TICK_CODE, rollback_tick_routine())):
            write_words(iso, addr, words, expect=[0] * len(words))
        buf = addi(3, 30, PAD_BUF - 0x803F0000)
        tail = POLL_TAIL
        write_words(iso, tail, [b(tail, RB_POLL_CODE, link=True)] + [nop()] * 5,
                    expect=[buf, b(tail + 4, POLL_CLEAR, link=True), buf,
                            b(tail + 12, PAD_READ, link=True), buf,
                            b(tail + 20, REBORN_REMAP, link=True)])
        write_words(iso, SIM1_SITE, [b(SIM1_SITE, RB_TICK_CODE, link=True)],
                    expect=[b(SIM1_SITE, SIM1, link=True)])
        write_words(iso, SIM2_SITE, [nop()], expect=[b(SIM2_SITE, SIM2, link=True)])
    if "netplay" in patches:
        write_words(iso, NETPLAY_STUB, [blr()], expect=[0])
        words = netplay_routine()
        write_words(iso, NETPLAY_CODE, words, expect=[0] * len(words))
        write_words(iso, POLL_CALL, [b(POLL_CALL, NETPLAY_CODE, link=True)],
                    expect=[b(POLL_CALL, INPUT_CODE, link=True)])

    # Apply only after the base recipe; original-instruction guards reject a
    # second application or a different game revision without publishing it.
    import draftmodes
    draftmodes.apply(iso, patches)


def build(src: str, dst: str, patches: set[str], progress=None) -> None:
    """Copy src to dst and patch the copy. progress(done_bytes, total) is called
    during the copy. On a PatchError the partial dst is removed."""
    check_patch_set(patches)
    total = os.path.getsize(src)
    try:
        with open(src, "rb") as fin, open(dst, "wb") as fout:
            done = 0
            while chunk := fin.read(16 << 20):
                fout.write(chunk)
                done += len(chunk)
                if progress:
                    progress(done, total)
        with open(dst, "r+b") as iso:
            apply_patches(iso, patches)
    except BaseException:
        try:
            os.remove(dst)
        except OSError:
            pass
        raise


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 2
    src, dst, patches = argv[0], argv[1], set(argv[2:])
    try:
        build(src, dst, patches)
    except PatchError as e:
        print(f"isopatch: {e}", file=sys.stderr)
        return 1
    print(f"wrote {dst} with {', '.join(sorted(patches))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
