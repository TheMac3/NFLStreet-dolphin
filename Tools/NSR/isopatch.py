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
  seed    Make fn_801CF7AC (the shared OSGetTick RNG seed source) return a
          constant, 0x13579BDF.
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

Usage: tools/isopatch.py SRC.iso DST.iso freeze [seed | seedlog] [input [netplay]] [profile | stress]
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
SEED = 0x13579BDF
SEEDLOG_CODE = 0x80411960    # after the input routine
NETPLAY_STUB = 0x804119AC     # must match NSRNetplay::STUB_ADDRESS
NETPLAY_CODE = 0x804119B0
INPUT_LOOP = 0x80411F08      # script loop start S, +4 = loop length L (0 = no loop)
SEEDLOG = 0x80411F10         # count; entries at +0x10, 16 bytes each
SEEDLOG_MAX = 14
PROFILE_CODE = 0x80411500
PROFILE_ACC = 0x804117C0
STRESS_K = 0x804117F0
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


def rlwinm(ra, rs, sh, mb, me): return (21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1)


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


class PatchError(Exception):
    """The image isn't the one these patches were written for (or bad options)."""


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


PATCHES = {"freeze", "seed", "seedlog", "input", "profile", "stress", "netplay"}
NETPLAY_SET = ("freeze", "seed", "input", "netplay")  # what friends' netplay images use


def check_patch_set(patches: set[str]) -> None:
    if not patches or not patches <= PATCHES:
        raise PatchError(f"unknown patches: {sorted(patches - PATCHES)}")
    for a, b_ in (("seed", "seedlog"), ("profile", "stress")):
        if {a, b_} <= patches:
            raise PatchError(f"{a} and {b_} can't be combined")
    for need, by in (("freeze", "seedlog"), ("freeze", "input"), ("input", "netplay")):
        if by in patches and need not in patches:
            raise PatchError(f"{by} needs {need}")


def apply_patches(iso, patches: set[str]) -> None:
    """Patch an already-copied image file (opened r+b) in place."""
    check_patch_set(patches)
    if "freeze" in patches:
        routine = freeze_routine()
        write_words(iso, FREEZE_CODE, routine, expect=[0] * len(routine))
        write_words(iso, FREEZE_COUNTER, [0, 0], expect=[0, 0])
        write_words(iso, DISPATCH_CALL, [b(DISPATCH_CALL, FREEZE_CODE, link=True)],
                    expect=[b(DISPATCH_CALL, ORIGINAL_CALLEE, link=True)])
    if "seed" in patches:
        write_words(iso, SEED_FN, [lis(3, SEED >> 16), ori(3, 3, SEED & 0xFFFF), blr()],
                    expect=[0x9421FFF8, 0x7C0802A6, 0x9001000C])
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
    if "netplay" in patches:
        write_words(iso, NETPLAY_STUB, [blr()], expect=[0])
        words = netplay_routine()
        write_words(iso, NETPLAY_CODE, words, expect=[0] * len(words))
        write_words(iso, POLL_CALL, [b(POLL_CALL, NETPLAY_CODE, link=True)],
                    expect=[b(POLL_CALL, INPUT_CODE, link=True)])


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
