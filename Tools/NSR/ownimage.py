#!/usr/bin/env python3
"""Read-only retail verification and evidence inventories for the ownimage study.

No game bytes are emitted. `verify` distinguishes raw retail, scrubbed retail
and modified dumps using executable, system and per-file references; `inventory` reports
DOL sections. An optional local importer manifest supplies *reported* input
metadata, not an independently verified retail baseline. See docs/ownimage.md.
The one named loading-screen variant is allowlisted by exact size and hash;
no other conflict is accepted or silently enrolled.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import discpatch
from ownimage_reference import REFERENCE


# Redump-style metadata, independently corroborated by GameTDB/TASVideos SHA-1.
# The DOL identity is from the decomp project's pinned target, not a disc hash.
RETAIL = {
    "size": 1459978240,
    "md5": "6ac216d88f4c833b0cb2fd9424264dce",
    "sha1": "d8603281a9a84a525ac7ae3dddbc30ea4f6bb190",
    "dol_sha1": "3561e946e9ff785b68692f87d2ba85d81fd2dcf4",
}
METADATA_URL = "https://redump.info/disc/11496"
MAX_DOL = 16 << 20

# Mason's 2026-10-24 provenance decision: the installer replaced only this
# loading-screen payload. This is a named variant, never a generic bypass.
# The retail reference remains unchanged and all other references stay exact.
VARIANTS = {"retail+reborn-loaddata": {
    "path": "loaddata.dat", "size": 4832256,
    "sha256": "adeac68bab482a626182b2642f140135a3d79739aa7cf4427f2f78abe00e2857",
}}


def hashes(stream, size: int) -> dict:
    """Hash an exact extent with bounded memory; reject truncation explicitly."""
    digests = {name: hashlib.new(name) for name in ("md5", "sha1", "sha256")}
    remaining = size
    while remaining:
        chunk = stream.read(min(remaining, 16 << 20))
        if not chunk:
            raise ValueError("truncated image extent")
        for digest in digests.values():
            digest.update(chunk)
        remaining -= len(chunk)
    return {name: digest.hexdigest() for name, digest in digests.items()}


def header(stream, size: int) -> bytes:
    """Reject compressed containers, other games and other revisions early."""
    stream.seek(0)
    data = stream.read(0x440)
    if len(data) != 0x440 or data[0x1C:0x20] != bytes.fromhex("c2339f3d"):
        raise ValueError("expected an uncompressed GameCube ISO/GCM dump")
    if data[:8] != b"GN7E69\0\0":
        raise ValueError("expected GN7E69 disc 0 revision 0")
    if size < 0x440:
        raise ValueError("truncated disc header")
    return data


def dol_metadata(stream, disc_header: bytes, size: int) -> dict:
    """Bound DOL file/guest extents before hashing; emit section metadata only."""
    offset = struct.unpack_from(">I", disc_header, 0x420)[0]
    if offset < 0x440 or offset + 0x100 > size:
        raise ValueError("invalid DOL header extent")
    stream.seek(offset)
    data = stream.read(0x100)
    offsets = struct.unpack_from(">18I", data, 0)
    addresses = struct.unpack_from(">18I", data, 0x48)
    sizes = struct.unpack_from(">18I", data, 0x90)
    sections = []
    for i, (start, address, length) in enumerate(zip(offsets, addresses, sizes)):
        if not length:
            continue
        if (start < 0x100 or start + length > MAX_DOL or offset + start + length > size
                or address < 0x80000000 or address + length > 0x81800000):
            raise ValueError("invalid DOL section extent")
        sections.append({"name": f"text{i}" if i < 7 else f"data{i - 7}",
                         "file_offset": start, "address": address, "size": length})
    extents = sorted((s["file_offset"], s["file_offset"] + s["size"]) for s in sections)
    if not sections or any(a[1] > b[0] for a, b in zip(extents, extents[1:])):
        raise ValueError("empty or overlapping DOL sections")
    guest_extents = sorted((s["address"], s["address"] + s["size"]) for s in sections)
    if any(a[1] > b[0] for a, b in zip(guest_extents, guest_extents[1:])):
        raise ValueError("overlapping DOL guest sections")
    for section in sections:
        stream.seek(offset + section["file_offset"])
        section["sha256"] = hashes(stream, section["size"])["sha256"]
    length = max(end for _, end in extents)
    stream.seek(offset)
    return {"offset": offset, "size": length, "sections": sections,
            "bss_address": struct.unpack_from(">I", data, 0xD8)[0],
            "bss_size": struct.unpack_from(">I", data, 0xDC)[0],
            "entry": struct.unpack_from(">I", data, 0xE0)[0],
            **hashes(stream, length)}


def system_metadata(stream, data: bytes, size: int) -> dict:
    """Hash bounded boot/system extents; reject overlap with the DOL/FST."""
    stream.seek(0x2440)
    app = stream.read(32)
    if len(app) != 32:
        raise ValueError("truncated apploader header")
    app_size = 32 + sum(struct.unpack_from(">II", app, 20))
    dol_offset, fst_offset, fst_size = struct.unpack_from(">III", data, 0x420)
    if app_size > 2 << 20 or 0x2440 + app_size > min(dol_offset, fst_offset, size):
        raise ValueError("invalid apploader extent")
    result = {}
    for name, offset, length in (("boot.bin", 0, 0x440), ("bi2.bin", 0x440, 0x2000),
                                 ("apploader.img", 0x2440, app_size),
                                 ("fst.bin", fst_offset, fst_size)):
        if length <= 0 or offset + length > size:
            raise ValueError("invalid system extent")
        stream.seek(offset)
        result[name] = {"offset": offset, "size": length, **hashes(stream, length)}
    return result


def retail_reference() -> dict:
    """Load metadata with explicit provenance; this file contains no payloads."""
    return REFERENCE


def verify(image: str | Path, reference: dict | None = None) -> dict:
    """Classify a dump without confusing padding loss with changed game files.

    Raw disc identity is authoritative. Scrubbed acceptance additionally needs
    the exact retail DOL, apploader, FST paths/sizes and every known file hash.
    Missing references and conflicting hashes fail closed. The only alternative
    is an exact named loading-screen variant, retained in the receipt identity. The
    receipt is input evidence, never rollback qualification.
    """
    reference = retail_reference() if reference is None else reference
    audit = inventory(image)
    with open(image, "rb") as stream:
        size = stream.seek(0, 2)
        data = header(stream, size)
        if size != RETAIL["size"]:
            raise ValueError("expected full 1459978240-byte dump; convert your own disc first")
        systems = system_metadata(stream, data, size)
        stream.seek(0)
        identity = hashes(stream, size)
    raw_match = all(identity[key] == RETAIL[key] for key in ("md5", "sha1"))
    conflicts, enrolled = [], []
    variant = "retail"
    if audit["dol"]["sha1"] != RETAIL["dol_sha1"]:
        conflicts.append({"path": "main.dol", "reason": "retail DOL identity differs"})
    expected = {row["path"]: row for row in reference["files"]}
    actual = {row["path"]: row for row in audit["files"]}
    for name in sorted(set(expected) | set(actual)):
        row, original = actual.get(name), expected.get(name)
        if row is None or original is None:
            conflicts.append({"path": name, "reason": "FST structure differs"})
        elif any(all(row[k] == known[k] for k in ("path", "size", "sha256"))
                 for known in VARIANTS.values()):
            variant = next(name for name, known in VARIANTS.items()
                           if all(row[k] == known[k] for k in ("path", "size", "sha256")))
        elif row["size"] != original["size"] or (original.get("sha256") and row["sha256"] != original["sha256"]):
            conflicts.append({"path": name, "reason": "file size/hash differs from reference",
                              "expected_size": original["size"], "actual_size": row["size"],
                              "expected_sha256": original.get("sha256"), "actual_sha256": row["sha256"]})
        elif not original.get("sha256"):
            conflicts.append({"path": name, "reason": "missing exact file hash reference"})
    if list(actual) != reference.get("fst_order", list(expected)):
        conflicts.append({"path": "fst.bin", "reason": "FST entry order differs from baseline"})
    for name, original in reference["systems"].items():
        row = systems.get(name)
        if row is None or any(row[k] != original[k] for k in ("size", "sha256")):
            conflicts.append({"path": name, "reason": "system size/hash differs from baseline"})
    # The pinned reference currently describes a flat FST. Extra empty
    # directories must not slip through merely because files() returns files.
    with open(image, "rb") as stream:
        stream.seek(systems["fst.bin"]["offset"])
        fst = stream.read(systems["fst.bin"]["size"])
    count = struct.unpack_from(">I", fst, 8)[0]
    if count != len(actual) + 1:
        conflicts.append({"path": "fst.bin", "reason": "unexpected directory structure"})
    classification = ("modified" if conflicts else variant if variant != "retail" else
                      "retail" if raw_match else "retail-scrubbed")
    return {"format": "nsrb.ownimage-verification.v3", "disc_id": "GN7E69",
            "disc": 0, "revision": 0, "size": size, **identity,
            "classification": classification, "variant": variant if not conflicts else None,
            "accepted": classification != "modified",
            "dol_sha1": audit["dol"]["sha1"], "systems": systems,
            "conflicts": conflicts, "enrolled_files": enrolled,
            "files": [{k: row[k] for k in ("path", "size", "sha256")} for row in audit["files"]],
            "reference_evidence": {k: reference.get(k) for k in ("files_evidence", "fst_evidence")},
            "system_evidence": {k: v.get("evidence", "reference") for k, v in reference["systems"].items()},
            "metadata": METADATA_URL, "rollback_qualified": False}


def inventory(image: str | Path, baseline_manifest: str | None = None) -> dict:
    """Describe every FST file and DOL section, without exporting game contents.

    Importer manifests omit system files and may describe normalized inputs.
    Keep that evidence limitation in machine-readable output and use neutral
    status labels instead of calling every unlisted file a retail addition.
    """
    baseline = {}
    if baseline_manifest:
        manifest = json.loads(Path(baseline_manifest).read_text(encoding="utf-8-sig"))
        for row in manifest["source_files"]:
            path = row["path"]
            if not path.startswith("files/") or path[6:] in baseline:
                raise ValueError("invalid or duplicate importer manifest file")
            baseline[path[6:]] = row
    entries = discpatch.files(image)
    with open(image, "rb") as stream:
        size = stream.seek(0, 2)
        data = header(stream, size)
        dol = dol_metadata(stream, data, size)
        result = []
        for name, (offset, length) in entries.items():
            stream.seek(offset)
            digest = hashes(stream, length)["sha256"]
            original = baseline.get(name)
            status = ("unlisted" if original is None else
                      "unchanged" if (digest, length) == (original["sha256"], original["bytes"])
                      else "changed")
            result.append({"path": name, "offset": offset, "size": length,
                           "sha256": digest, "reported_input": original, "status": status})
    return {"format": "nsrb.ownimage-inventory.v1", "disc_id": "GN7E69", "revision": 0,
            "size": size, "dol": dol, "files": result,
            "reported_input_only": sorted(set(baseline) - set(entries)),
            "baseline_evidence": "local importer manifest; not an independently verified clean disc"
                                 if baseline_manifest else "none"}


def main(argv=None) -> int:
    """Print metadata to stdout so callers can retain evidence under local/."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    subs = parser.add_subparsers(dest="command", required=True)
    subs.add_parser("verify").add_argument("image")
    audit = subs.add_parser("inventory")
    audit.add_argument("image")
    audit.add_argument("--baseline-manifest", help="read-only local importer input file metadata")
    delta = subs.add_parser("compare")
    delta.add_argument("image")
    delta.add_argument("other")
    args = parser.parse_args(argv)
    try:
        if args.command == "compare":
            from ownimage_compare import compare
            result = compare(args.image, args.other)
        else:
            result = (verify(args.image) if args.command == "verify"
                      else inventory(args.image, args.baseline_manifest))
        print(json.dumps(result, indent=2))
        if args.command == "verify" and not result["accepted"]:
            return 1
    except (OSError, ValueError, KeyError, struct.error) as exc:
        print(f"ownimage: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
