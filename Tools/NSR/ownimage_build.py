#!/usr/bin/env python3
"""Authored GN7E69-r0 retail bootstrap and transactional local image recipe.

Only metadata and our PPC encoders are distributed. The player's disc is read
only. The production entry point refuses conflicting retail references; the
low-level bootstrap is also usable by synthetic tests/private lab probes and
always guards the exact retail executable. See docs/ownimage.md for provenance
limits and qualification status. No Reborn implementation is used here.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import struct
import tempfile

import discpatch
import eadb
import isopatch as pp
import ownimage

FAMILY = "gn7e69-r0-retail"
RECIPE = "gn7e69-r0-ownimage-rollback-v1"
PAD_CLAMP = 0x802465BC
ARENA_SET = 0x80239A10
ARENA_SDA = -0x6148
ARENA_MIN = 0x80417000
ARENA_CODE = 0x80411000
PATCH_BASE = 0x80410000
MARKER = (0x4E53524F, 1)  # NSRO, independently authored recipe ABI version


def arena_routine() -> list[int]:
    """OSSetArenaLo(low): store max(low, patch end), preserving higher bounds.

    Both native OSInit paths and later setters obey this lower bound. The
    original main-thread stack ends at 803FE4C0; it remains below our section.
    The adapter uses only volatile registers and cr0, with no stack or calls.
    """
    return [pp.lis(12, ARENA_MIN >> 16), pp.ori(12, 12, ARENA_MIN & 0xFFFF),
            pp.cmplw(3, 12), pp.bc(ARENA_CODE + 12, ARENA_CODE + 20, 4, 0),
            pp.mr(3, 12), pp.stw(3, ARENA_SDA, 13), pp.blr()]


def free_extent(size: int, occupied: list[tuple[int, int]], length: int) -> int:
    """Choose an aligned unreferenced disc span; never overwrite FST/system data."""
    prior = 0
    for lo, hi in sorted(occupied) + [(size, size)]:
        candidate = (prior + 31) & ~31
        if candidate + length <= lo:
            return candidate
        prior = max(prior, hi)
    raise pp.PatchError("no unreferenced disc extent can hold the expanded DOL")


def bootstrap(stream, entries: dict[str, tuple[int, int]]) -> dict:
    """Relocate the exact retail DOL and add zero-initialized authored sections.

    This bounded low-level operation checks executable identity, layout, stack,
    unused section slots and displaced words. The caller owns asset provenance;
    build() enforces it before making a copy. Gaps between referenced disc
    extents are layout space, never treated as executable/data references.
    """
    size = stream.seek(0, 2)
    data = ownimage.header(stream, size)
    dol = ownimage.dol_metadata(stream, data, size)
    systems = ownimage.system_metadata(stream, data, size)
    if dol["sha1"] != ownimage.RETAIL["dol_sha1"]:
        raise pp.PatchError("bootstrap requires the exact retail DOL")
    for section in dol["sections"]:
        if section["address"] + section["size"] > 0x803EE4C0:
            raise pp.PatchError("retail section overlaps the verified stack/patch layout")
    if dol["bss_address"] + dol["bss_size"] > 0x803EE4C0:
        raise pp.PatchError("retail BSS overlaps the stack/patch layout")
    stream.seek(dol["offset"])
    expanded = bytearray(stream.read(dol["size"]))
    occupied = [(0, systems["apploader.img"]["offset"] + systems["apploader.img"]["size"]),
                (dol["offset"], dol["offset"] + dol["size"]),
                (systems["fst.bin"]["offset"], systems["fst.bin"]["offset"] + systems["fst.bin"]["size"])]
    occupied += [(offset, offset + length) for offset, length in entries.values() if length]
    for slot, address, length in ((2, PATCH_BASE, 0x6B00), (12, pp.AUDIO_CLOCK_STATE, 0x500)):
        if any(struct.unpack_from(">I", expanded, base + 4 * slot)[0] for base in (0, 0x48, 0x90)):
            raise pp.PatchError("bootstrap section slot is occupied")
        offset = (len(expanded) + 31) & ~31
        expanded.extend(bytes(offset + length - len(expanded)))
        for base, value in ((0, offset), (0x48, address), (0x90, length)):
            struct.pack_into(">I", expanded, base + 4 * slot, value)
    destination = free_extent(size, occupied, len(expanded))
    stream.seek(destination)
    stream.write(expanded)
    stream.seek(0x420)
    stream.write(struct.pack(">I", destination))
    pp.write_words(stream, PATCH_BASE, list(MARKER), expect=[0, 0])
    words = arena_routine()
    pp.write_words(stream, ARENA_CODE, words, expect=[0] * len(words))
    pp.write_words(stream, ARENA_SET, [pp.b(ARENA_SET, ARENA_CODE)],
                   expect=[pp.stw(3, ARENA_SDA, 13)])
    # A tail branch preserves PADClamp's native four-port behavior and LR.
    # Raw pads are exchanged before this adapter, processed pads captured after.
    pp.write_words(stream, pp.REBORN_REMAP, [pp.b(pp.REBORN_REMAP, PAD_CLAMP)], expect=[0])
    return {"dol_offset": destination, "dol_size": len(expanded),
            "patch_lo": PATCH_BASE, "arena_min": ARENA_MIN, "family": FAMILY, "recipe": RECIPE}


def build(source: str | Path, output: str | Path, progress=None) -> dict:
    """Verify, copy, bootstrap, patch and atomically publish under local/.

    No force option exists. Every referenced payload must retain its input
    hash. Exclusive publication and inode checks protect source aliases and
    previous outputs; copy hashing detects a source changed after verification.
    """
    source = Path(source).resolve()
    output = eadb.local_output(output)
    if output.exists() or output.is_symlink() or output == source:
        raise pp.PatchError("retail output must be a new local image")
    receipt = ownimage.verify(source)
    if not receipt["accepted"]:
        names = ", ".join(row["path"] for row in receipt["conflicts"])
        raise pp.PatchError(f"modified retail source: conflicting references: {names}")
    entries = discpatch.files(source)
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".ownimage-", suffix=".iso", dir=output.parent)
    try:
        digest = hashlib.sha256()
        with os.fdopen(fd, "w+b") as target, source.open("rb") as src:
            done = 0
            while chunk := src.read(16 << 20):
                target.write(chunk)
                digest.update(chunk)
                done += len(chunk)
                if progress:
                    progress(done, receipt["size"])
            if digest.hexdigest() != receipt["sha256"]:
                raise pp.PatchError("retail source changed while copying")
            layout = bootstrap(target, entries)
            pp.apply_patches(target, set(pp.ROLLBACK_SET), poll_original=PAD_CLAMP, retail=True)
            target.flush()
            os.fsync(target.fileno())
        audit = ownimage.inventory(temporary)
        if [{k: r[k] for k in ("path", "size", "sha256")} for r in audit["files"]] != receipt["files"]:
            raise pp.PatchError("retail payload verification failed")
        result = {**layout, "source_sha256": receipt["sha256"],
                  "source_classification": receipt["classification"],
                  "output_sha256": discpatch.sha256(temporary), "rollback_qualified": False}
        os.link(temporary, output)
        return result
    finally:
        os.unlink(temporary)
