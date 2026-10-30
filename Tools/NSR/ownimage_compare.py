#!/usr/bin/env python3
"""Metadata-only FST/system/mapped-DOL delta for two private GN7E69-r0 images.

The first image is an observed baseline, not implicitly an authenticated retail
source. Changed address ranges and hashes are emitted without instruction or
asset bytes. Standard REL headers are checked at file/known TERF entry starts;
opaque/compressed containers remain explicitly outside that REL coverage.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import struct

import eadb
import ownimage


def changed_ranges(left: bytes, right: bytes, address: int) -> dict:
    """Coalesce changed aligned words and count exact byte differences."""
    spans, start = [], None
    for offset in range(0, len(left), 4):
        different = left[offset:offset + 4] != right[offset:offset + 4]
        if different and start is None:
            start = offset
        if start is not None and (not different or offset + 4 >= len(left)):
            end = offset if not different else len(left)
            spans.append({"lo": address + start, "hi": address + end,
                          "before_sha256": hashlib.sha256(left[start:end]).hexdigest(),
                          "after_sha256": hashlib.sha256(right[start:end]).hexdigest()})
            start = None
    return {"bytes_changed": sum(a != b for a, b in zip(left, right)), "ranges": spans}


def rel_header(data: bytes) -> bool:
    """Recognize a bounded standard REL module, not an arbitrary code-like word."""
    if len(data) < 0x40:
        return False
    module, next_, prev, count, sections, names, name_size, version = struct.unpack_from(">8I", data)
    if not module or next_ or prev or not 1 <= count <= 32 or version not in (1, 2, 3):
        return False
    minimum = (0x40, 0x48, 0x4C)[version - 1]
    if sections < minimum or sections % 4 or sections + 8 * count > len(data):
        return False
    if name_size and (names < minimum or names + name_size > len(data)):
        return False
    present = 0
    for i in range(count):
        offset, size = struct.unpack_from(">II", data, sections + i * 8)
        offset &= ~3
        if offset:
            if offset < minimum or offset + size > len(data):
                return False
            present += 1
    return present > 0


def containers(image: str | Path, files: list[dict]) -> list[dict]:
    """Hash all bounded TERF children and report REL coverage without exporting bytes."""
    result = []
    with open(image, "rb") as stream:
        for row in files:
            stream.seek(row["offset"])
            magic = stream.read(min(row["size"], 4))
            entry = {"path": row["path"], "magic_hex": magic.hex(), "children": [],
                     "rel_coverage": "opaque; no nested REL absence claim"}
            if row["size"] <= 64 << 20:
                stream.seek(row["offset"])
                data = stream.read(row["size"])
                entry["standard_rel_at_file_start"] = rel_header(data)
                if magic == b"TERF" and data[0x40:0x44] == b"DIR1":
                    for index, (offset, size) in enumerate(eadb.terf_entries(data)):
                        child = data[offset:offset + size]
                        entry["children"].append({"index": index, "offset": offset, "size": size,
                                                  "sha256": hashlib.sha256(child).hexdigest(),
                                                  "standard_rel": rel_header(child)})
                    entry["rel_coverage"] = "file start and every TERF/DIR1 entry start"
            result.append(entry)
    return result


def compare(baseline: str | Path, other: str | Path) -> dict:
    """Compare every named file/system extent and every original mapped DOL section."""
    audits = [ownimage.inventory(path) for path in (baseline, other)]
    files = [{r["path"]: r for r in audit["files"]} for audit in audits]
    rows = []
    for name in sorted(set(files[0]) | set(files[1])):
        a, b = files[0].get(name), files[1].get(name)
        status = "added" if a is None else "removed" if b is None else (
            "unchanged" if (a["size"], a["sha256"]) == (b["size"], b["sha256"]) else "changed")
        rows.append({"path": name, "status": status, "before": a, "after": b})
    systems = []
    for path in (baseline, other):
        with open(path, "rb") as stream:
            size = stream.seek(0, 2)
            systems.append(ownimage.system_metadata(stream, ownimage.header(stream, size), size))
    dol_rows = []
    with open(baseline, "rb") as left, open(other, "rb") as right:
        for a in audits[0]["dol"]["sections"]:
            b = next((r for r in audits[1]["dol"]["sections"] if (r["address"], r["size"]) ==
                      (a["address"], a["size"])), None)
            if b is None:
                dol_rows.append({"before": a, "status": "removed-or-resized"})
                continue
            left.seek(audits[0]["dol"]["offset"] + a["file_offset"])
            right.seek(audits[1]["dol"]["offset"] + b["file_offset"])
            delta = changed_ranges(left.read(a["size"]), right.read(b["size"]), a["address"])
            dol_rows.append({"before": a, "after": b, **delta})
    original_spans = {(r["address"], r["size"]) for r in audits[0]["dol"]["sections"]}
    return {"format": "nsrb.ownimage-delta.v1", "disc_id": "GN7E69", "revision": 0,
            "baseline_evidence": "observed operator-supplied image; see its verification receipt",
            "files": rows, "systems_before": systems[0], "systems_after": systems[1],
            "dol_before": audits[0]["dol"], "dol_after": audits[1]["dol"], "dol_sections": dol_rows,
            "dol_added_sections": [r for r in audits[1]["dol"]["sections"]
                                   if (r["address"], r["size"]) not in original_spans],
            "standalone_rel_paths": [[r["path"] for r in a["files"] if r["path"].lower().endswith(".rel")]
                                      for a in audits],
            "containers_before": containers(baseline, audits[0]["files"]),
            "containers_after": containers(other, audits[1]["files"])}
