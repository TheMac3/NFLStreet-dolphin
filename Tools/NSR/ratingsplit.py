"""Select runtime split releases and retain the legacy GN7E69-r0 PPVL builder.

New split releases use an agreed v3 boot bundle and leave the image unchanged.
For legacy local PPVL image builds only, PLAY bit 790 (seven bits, no EA
descriptor) compiles into isopatch's bounded name-hash table. No game bytes or
roster rows are distributed. See docs/ratingsplit.md.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import struct
import tempfile

import discpatch
import eadb
import isopatch as ip

RECIPE = "gn7e69-r0-rollback-ratingsplit-v1"  # legacy opt-in baked PPVL
RUNTIME_RECIPE = "gn7e69-r0-rollback-ratingsplit-runtime-v2"


def name_hash(first: str, last: str) -> int:
    """FNV-1a over exact Latin-1 name bytes separated by 0xff, matching PPC."""
    value = 0x811C9DC5
    for byte in first.encode("latin-1") + b"\xff" + last.encode("latin-1"):
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def table_bytes(template: bytes) -> bytes:
    """Compile a bounded table; hash collisions and conflicting copies fail closed."""
    table = eadb.roster_tables(template)["PLAY"]
    if "PPVL" not in {f.name for f in table.fields}:
        raise ValueError("ratingsplit requires the verified 100-byte PLAY layout")
    values, names = {}, {}
    for row in table.records:
        name = (row["PFNA"], row["PLNA"])
        key, value = name_hash(*name), row["PPVL"]
        if not 0 <= value <= 99:
            raise ValueError("invalid ratingsplit velocity")
        if key in names and names[key] != name:
            raise ValueError("ratingsplit name hash collision")
        if key in values and values[key] != value:
            raise ValueError("conflicting PPVL copies for a player name")
        names[key], values[key] = name, value
    entries = sorted((key, value) for key, value in values.items() if value)
    if len(entries) > ip.RATINGSPLIT_CAPACITY:
        raise ValueError("ratingsplit table capacity exceeded")
    raw = struct.pack(">II", ip.RATINGSPLIT_MAGIC, len(entries))
    raw += b"".join(struct.pack(">II", key, value * 255 // 99) for key, value in entries)
    return raw.ljust(8 + 8 * ip.RATINGSPLIT_CAPACITY, b"\0")


def enabled(rows: list[dict]) -> bool:
    """Select the feature recipe for an explicit PPVL edit, including removal."""
    return any(row.get("field") == "PPVL" for row in rows)


def runtime_splits(rows: list[dict]) -> bool:
    """The signed release uses v3 runtime semantics for either split."""
    return any(row.get("field") in ("PPVL", "PCEL") for row in rows)


def requires_runtime(rows: list[dict]) -> bool:
    """PCEL's new acceleration consumer requires the v3 runtime policy."""
    return any(row.get("field") == "PCEL" for row in rows)


def plan(image: str | Path, before: bytes, after: bytes) -> list[tuple[int, bytes, bytes]]:
    """Expect-check the complete cave and return offset/old/new patches.

    Existing feature images can be edited again; their table must agree with
    their source roster. Zero-padding is checked over the entire reserved table.
    Caller owns the fresh output, never the input image.
    """
    new_table = table_bytes(after)
    routine = struct.pack(f">{len(ip.ratingsplit_routine())}I", *ip.ratingsplit_routine())
    branch = struct.pack(">I", ip.b(ip.RATINGSPLIT_SITE, ip.RATINGSPLIT_CODE))
    original = struct.pack(">I", (42 << 26) | (31 << 16) | 0xBC4)
    result = []
    with open(image, "rb") as stream:
        header = stream.read(8)
        if header[:6] != b"GN7E69" or header[7] != 0:
            raise ValueError("ratingsplit requires GN7E69 revision 0")
        site = ip.dol_va_to_iso(stream, ip.RATINGSPLIT_SITE)
        stream.seek(site)
        actual = stream.read(4)
        installed = actual == branch
        if actual not in (original, branch):
            raise ValueError("ratingsplit hook conflicts with another patch")
        if not installed and new_table[4:8] == bytes(4):
            return []
        old_table = table_bytes(before) if installed else bytes(len(new_table))
        for address, old, new in [(ip.RATINGSPLIT_SITE, branch if installed else original, branch),
                                  (ip.RATINGSPLIT_CODE, routine if installed else bytes(len(routine)), routine),
                                  (ip.RATINGSPLIT_TABLE, old_table, new_table)]:
            offset = ip.dol_va_to_iso(stream, address)
            if ip.dol_va_to_iso(stream, address + len(new) - 1) != offset + len(new) - 1:
                raise ValueError("ratingsplit patch crosses DOL sections")
            stream.seek(offset)
            if stream.read(len(old)) != old:
                raise ValueError(f"ratingsplit cave conflict at {address:#x}")
            result.append((offset, old, new))
    return sorted(result)


def apply_plan(image: str | Path, patches: list[tuple[int, bytes, bytes]]) -> None:
    """Apply a prevalidated plan only to the caller's private temporary image."""
    with open(image, "r+b") as stream:
        for offset, old, _ in patches:
            stream.seek(offset)
            if stream.read(len(old)) != old:
                raise ValueError("ratingsplit image changed after planning")
        for offset, _, new in patches:
            stream.seek(offset)
            stream.write(new)
        stream.flush()
        os.fsync(stream.fileno())


def predicted_sha256(image: str | Path, roster_file: str, before: bytes, after: bytes) -> str:
    """Hash exactly the DOL and roster replacements without writing game data."""
    patches = plan(image, before, after)
    offset, size = discpatch.files(image)[roster_file]
    if size != len(after) or discpatch.read_file(image, roster_file) != before:
        raise ValueError("ratingsplit source roster changed")
    patches = sorted([*patches, (offset, before, after)])
    digest, cursor = hashlib.sha256(), 0
    with open(image, "rb") as stream:
        for offset, old, new in patches:
            if offset < cursor or len(old) != len(new):
                raise ValueError("overlapping ratingsplit patches")
            remaining = offset - cursor
            while remaining:
                chunk = stream.read(min(remaining, 16 << 20))
                if not chunk:
                    raise ValueError("truncated ratingsplit image")
                digest.update(chunk)
                remaining -= len(chunk)
            digest.update(new)
            cursor = offset + len(old)
            stream.seek(cursor)
        while chunk := stream.read(16 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def build_copy(source: Path, output: Path, roster_file: str, before: bytes, after: bytes,
               base_sha256: str) -> str:
    """Atomically publish a fresh editor build after roster and DOL verification."""
    output = eadb.local_output(output)
    patches = plan(source, before, after)
    if output.exists():
        raise ValueError("ratingsplit output already exists")
    with tempfile.TemporaryDirectory(prefix=".ratingsplit-", dir=output.parent) as directory:
        temporary = Path(directory) / "build.iso"
        discpatch.patch_copy(source, temporary, roster_file, after, expect=before,
                             expect_image_sha256=base_sha256)
        apply_plan(temporary, patches)
        digest = discpatch.sha256(temporary)
        if digest != predicted_sha256(source, roster_file, before, after):
            raise ValueError("ratingsplit output fingerprint differs from prediction")
        os.link(temporary, output)
    return digest
