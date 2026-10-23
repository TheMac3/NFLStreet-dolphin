#!/usr/bin/env python3
"""eadb.py: parse and edit NFL Street 2 (GN7E69) roster attributes.

Two formats, both big-endian (GameCube):

* **EA "DB" table database**: the same family as Madden-era `.db`/TDB
  rosters. Used directly by `Streamed.db` and embedded inside TERF files.
  Layout, as worked out in docs/research-log.md (2026-10-07):

    header (0x18): "DB" u16(8) u32 u32(file size) u32 u32(table count) u32(crc)
    index: table_count x (tag[4], offset u32); offsets are relative to the end
           of the index
    table header (0x28): crc, ?, record_bytes, ?, ?, u16 capacity, u16 count,
           u32 (0x0000FFFF), u8 field_count, u8 index_count, u16, u32, crc
    field defs: field_count x (type u32, bit_offset u32, tag[4], bit_width u32)
    records: count x record_bytes, fields bit-packed MSB-first

  Tags are stored byte-reversed ("YALP" on disc is table PLAY).

* **TERF container**: "TERF" header (0x40), then a "DIR1" chunk at 0x40 holding
  (offset, size) pairs. Offsets are relative to the end of DIR1. Size-0 entries
  are empty slots. In `db_teams.dat` the 32 non-empty entries are one DB per team.

Field type codes seen so far: 0 = string (confirmed: player names), 3 = unsigned,
2 = signed (likely), 4 = float and 1 = blob (likely; both dumped as unsigned for now).

Usage:
  tools/eadb.py summary <file>               tables, fields, record counts
  tools/eadb.py dump <file> TABLE [--entry N] [--csv]
  tools/eadb.py export-editable <file> --out local/current.json [--csv]
  tools/eadb.py apply <file> <pack.csv> --out local/template-edited.dat
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path


def _tag(raw: bytes) -> str:
    """Decode the reversed four-byte identifiers used by EA's database."""
    return raw[::-1].decode("latin-1")


@dataclass
class Field:
    """One on-disc field descriptor; offsets and widths are in bits."""
    name: str
    type: int
    bit_offset: int
    bits: int


@dataclass
class Table:
    """Decoded rows plus absolute offsets for same-size record updates."""
    name: str
    record_bytes: int
    capacity: int
    count: int
    fields: list[Field]
    records: list[dict] = field(default_factory=list)
    records_offset: int = 0
    header_offset: int = 0


def _extract(rec: bytes, f: Field):
    """Decode one MSB-first field without changing adjacent packed fields."""
    if f.type == 0:  # string, byte-aligned
        start = f.bit_offset // 8
        return rec[start:start + f.bits // 8].split(b"\0", 1)[0].decode("latin-1")
    total = len(rec) * 8
    v = (int.from_bytes(rec, "big") >> (total - f.bit_offset - f.bits)) & ((1 << f.bits) - 1)
    if f.type == 2 and v >> (f.bits - 1):  # signed (guess): two's complement
        v -= 1 << f.bits
    return v


def parse_db(buf: bytes, base: int = 0) -> dict[str, Table]:
    """Parse one EA DB starting at buf[base]. Returns tables by name, in file order."""
    if base < 0 or base + 0x18 > len(buf) or buf[base:base + 2] != b"DB":
        raise ValueError(f"no DB magic at {base:#x}")
    (n_tables,) = struct.unpack_from(">I", buf, base + 0x10)
    data = base + 0x18 + n_tables * 8
    if data > len(buf):
        raise ValueError("truncated DB index")
    tables: dict[str, Table] = {}
    for i in range(n_tables):
        tag = _tag(buf[base + 0x18 + i * 8: base + 0x1C + i * 8])
        (off,) = struct.unpack_from(">I", buf, base + 0x1C + i * 8)
        t = data + off
        if t + 0x28 > len(buf) or tag in tables:
            raise ValueError(f"invalid table header: {tag}")
        rec_bytes = struct.unpack_from(">I", buf, t + 8)[0]
        capacity, count = struct.unpack_from(">HH", buf, t + 20)
        n_fields, n_index = buf[t + 28], buf[t + 29]
        if count > capacity or not rec_bytes or t + 0x28 + n_fields * 16 + count * rec_bytes > len(buf):
            raise ValueError(f"invalid table bounds: {tag}")
        if n_index:
            # Index sections haven't been seen yet; their layout is unknown, so
            # stop instead of misreading records. Revisit when a table has one.
            raise NotImplementedError(f"table {tag} has {n_index} index(es)")
        fields = []
        for j in range(n_fields):
            ftype, foff, ftag, fbits = struct.unpack_from(">II4sI", buf, t + 0x28 + j * 16)
            if not fbits or foff + fbits > rec_bytes * 8 or (ftype == 0 and (foff % 8 or fbits % 8)):
                raise ValueError(f"invalid field bounds: {tag}.{_tag(ftag)}")
            if any(f.name == _tag(ftag) or max(foff, f.bit_offset) < min(foff + fbits, f.bit_offset + f.bits) for f in fields):
                raise ValueError(f"overlapping or duplicate field: {tag}.{_tag(ftag)}")
            fields.append(Field(_tag(ftag), ftype, foff, fbits))
        rec0 = t + 0x28 + n_fields * 16
        recs = []
        for r in range(count):
            raw = buf[rec0 + r * rec_bytes: rec0 + (r + 1) * rec_bytes]
            recs.append({f.name: _extract(raw, f) for f in sorted(fields, key=lambda f: f.bit_offset)})
        tables[tag] = Table(tag, rec_bytes, capacity, count, fields, recs, rec0, t)
    return tables


def terf_entries(buf: bytes) -> list[tuple[int, int]]:
    """Return absolute (offset, size) for every TERF/DIR1 slot (size 0 = empty)."""
    if len(buf) < 0x48 or buf[:4] != b"TERF" or buf[0x40:0x44] != b"DIR1":
        raise ValueError("not a TERF/DIR1 file")
    (count,) = struct.unpack_from(">H", buf, 0x0E)
    (dir_size,) = struct.unpack_from(">I", buf, 0x44)
    base = 0x40 + dir_size
    if dir_size < 8 + count * 8 or base > len(buf):
        raise ValueError("invalid TERF directory size")
    out = []
    for i in range(count):
        off, size = struct.unpack_from(">II", buf, 0x48 + i * 8)
        if size and base + off + size > len(buf):
            raise ValueError(f"TERF entry {i} extends beyond file")
        out.append((base + off, size))
    return out


def load(path: str) -> list[tuple[int, dict[str, Table] | None]]:
    """Return [(entry_index, tables-or-None)]. A plain DB file is entry 0."""
    buf = open(path, "rb").read()
    if buf[:2] == b"DB":
        return [(0, parse_db(buf))]
    out = []
    for i, (off, size) in enumerate(terf_entries(buf)):
        out.append((i, parse_db(buf, off) if size and buf[off:off + 2] == b"DB" else None))
    return out



# The editor exposes known attributes only; identity/appearance fields and unknown
# type-1/type-4 encodings stay read-only until their semantics are established.
ATTRIBUTES = {
    "POVR": "Overall", "PSPD": "Speed", "PAGI": "Agility", "PCTH": "Catching",
    "PTAK": "Tackling", "PBLK": "Blocking", "PBTK": "Break tackle",
    "PCEL": "Acceleration", "PJUM": "Jumping", "PPSS": "Passing",
    "PCOV": "Coverage", "PWGT": "Weight (stored pounds minus 160)",
    "PHGT": "Height (inches)",
}
PACK_COLUMNS = ("first_name", "last_name", "field", "value", "expected",
                "template_sha256", "base_sha256")
LOCAL = Path(__file__).resolve().parents[1] / "local"


class PackError(ValueError):
    """All edit validation failures, including stale rows, reported together."""

    def __init__(self, errors):
        """Retain individual diagnostics for both CLI and HTTP callers."""
        self.errors = errors
        super().__init__("; ".join(errors))


def local_output(path: str | Path) -> Path:
    """Constrain generated data to local/, outside read-only source trees.

    Resolve symlinks before checking. Callers use exclusive creation for new
    artifacts so existing images, extracted inputs and hardlinks are never edited.
    """
    target = Path(path).absolute()
    resolved = target.resolve()
    root = LOCAL.resolve()
    if not resolved.is_relative_to(root) or resolved == root:
        raise ValueError("output must be under this repository's local/ directory")
    relative = resolved.relative_to(root)
    if relative.parts[0].lower() in {"reborn", "extracted", "discs"}:
        raise ValueError("output cannot be inside a read-only source tree")
    if resolved.name.lower() == "nsr.iso" or "content" in [p.lower() for p in relative.parts]:
        raise ValueError("output cannot target a Reborn install or NSR.iso")
    for parent in resolved.parents:
        if (parent / "content/games/NSR.iso").exists() or (parent / "app/content/games/NSR.iso").exists():
            raise ValueError("output cannot be inside a Reborn install")
        if parent == root:
            break
    if target.is_symlink():
        raise ValueError("output cannot be a symlink")
    return resolved


def roster_tables(buf: bytes, entry: int = 2) -> dict[str, Table]:
    """Select the live TERF DB, bounding parsing to its directory slot."""
    if buf[:2] == b"DB":
        return parse_db(buf)
    entries = terf_entries(buf)
    if not 0 <= entry < len(entries):
        raise ValueError(f"missing TERF entry {entry}")
    off, size = entries[entry]
    tables = parse_db(buf[off:off + size])
    for table in tables.values():
        table.records_offset += off
        table.header_offset += off
    return tables


def field_range(f: Field) -> tuple[int, int]:
    """Return storage bounds; signed fields use the parser's two's complement."""
    if f.type not in (2, 3) or not f.bits:
        raise ValueError(f"{f.name}: unsupported numeric field type")
    return (-(1 << (f.bits - 1)), (1 << (f.bits - 1)) - 1) if f.type == 2 else (0, (1 << f.bits) - 1)


def integer(value) -> int:
    """Require exact decimal integers, rejecting booleans and lossy floats."""
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ValueError("value must be an integer")
    return int(value)


def name_key(first: str, last: str) -> tuple[str, str]:
    """Case-insensitive exact names match all team copies, without fuzzy guesses."""
    return first.strip().casefold(), last.strip().casefold()


def read_pack(text: str) -> list[dict]:
    """Parse edit-only CSV; reject table exports and ambiguous column layouts."""
    reader = csv.DictReader(io.StringIO(text.lstrip("\ufeff")))
    headers = reader.fieldnames or []
    if (not set(PACK_COLUMNS[:4]).issubset(headers) or len(set(headers)) != len(headers)
            or set(headers) - set(PACK_COLUMNS)):
        raise ValueError("pack CSV requires first_name,last_name,field,value; optional expected,template_sha256,base_sha256")
    rows = list(reader)
    if any(None in row or any(v is None for v in row.values()) for row in rows):
        raise ValueError("malformed pack CSV row")
    return rows


def pack_csv(rows: list[dict]) -> str:
    """Serialize edits and provenance only, never an extracted player table."""
    out = io.StringIO(newline="")
    writer = csv.DictWriter(out, fieldnames=PACK_COLUMNS, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    return out.getvalue()


def apply_edits(buf: bytes, edits: list[dict], entry: int = 2,
                base_sha256: str | None = None) -> tuple[bytes, list[dict]]:
    """Validate every row before mutating; apply to all exact-name PLAY copies.

    expected is a pipe-separated set of original values (copies may differ).
    Hashes, when supplied, must match; a base hash requires the caller to supply
    the actual image hash. CRC words are preserved: Reborn already carries stale
    table CRCs and loads edited records unchanged (docs/roster-editing.md).
    No partial output is returned if any edit is stale or invalid.
    """
    tables = roster_tables(buf, entry)
    if "PLAY" not in tables:
        raise ValueError("selected database has no PLAY table")
    table = tables["PLAY"]
    fields = {f.name: f for f in table.fields}
    digest = hashlib.sha256(buf).hexdigest()
    errors, actions, seen = [], [], set()
    for line, row in enumerate(edits, 2):
        try:
            if not isinstance(row, dict) or set(row) - set(PACK_COLUMNS):
                raise ValueError("unknown edit columns")
            first, last = row["first_name"], row["last_name"]
            if not isinstance(first, str) or not isinstance(last, str) or not (first.strip() or last.strip()):
                raise ValueError("player name is required")
            key = name_key(first, last)
            tag = row["field"]
            if tag not in ATTRIBUTES or tag not in fields:
                raise ValueError(f"unsupported attribute {tag}")
            if (key, tag) in seen:
                raise ValueError("duplicate player/field edit")
            seen.add((key, tag))
            f = fields[tag]
            value = integer(row["value"])
            lo, hi = field_range(f)
            if not lo <= value <= hi:
                raise ValueError(f"{tag} must be {lo}..{hi}")
            matches = [i for i, r in enumerate(table.records)
                       if name_key(r.get("PFNA", ""), r.get("PLNA", "")) == key]
            if not matches:
                raise ValueError(f"stale: player {first} {last} was not found")
            old = sorted({table.records[i][tag] for i in matches})
            if str(row.get("expected", "")):
                expected = sorted({integer(v) for v in str(row["expected"]).split("|")})
                if old != expected:
                    raise ValueError(f"stale: {first} {last} {tag} expected {expected}, found {old}")
            if row.get("template_sha256") and row["template_sha256"] != digest:
                raise ValueError("stale: template SHA-256 differs")
            if row.get("base_sha256") and row["base_sha256"] != base_sha256:
                raise ValueError("stale: base image SHA-256 differs or was not supplied")
            canonical = table.records[matches[0]]
            actions.append((matches, f, value, old, canonical["PFNA"], canonical["PLNA"]))
        except (KeyError, TypeError, ValueError) as exc:
            errors.append(f"row {line}: {exc}")
    if errors:
        raise PackError(errors)
    out, report = bytearray(buf), []
    for matches, f, value, old, first, last in actions:
        shift = table.record_bytes * 8 - f.bit_offset - f.bits
        mask = ((1 << f.bits) - 1) << shift
        for i in matches:
            start = table.records_offset + i * table.record_bytes
            raw = int.from_bytes(out[start:start + table.record_bytes], "big")
            raw = (raw & ~mask) | ((value & ((1 << f.bits) - 1)) << shift)
            out[start:start + table.record_bytes] = raw.to_bytes(table.record_bytes, "big")
        report.append({"first_name": first, "last_name": last, "field": f.name,
                       "before": old, "value": value, "matches": len(matches)})
    return bytes(out), report


def editable(buf: bytes, entry: int = 2) -> dict:
    """Return private editor data grouped by name, including team membership."""
    tables = roster_tables(buf, entry)
    table = tables["PLAY"]
    fields = [{"name": f.name, "label": ATTRIBUTES[f.name], "min": field_range(f)[0],
               "max": field_range(f)[1], "bits": f.bits}
              for f in table.fields if f.name in ATTRIBUTES and f.type in (2, 3)]
    teams = {r["TGID"]: " ".join(str(r.get(k, "")).strip() for k in ("TDNA", "TLNA")).strip()
             for r in tables["TEAM"].records} if "TEAM" in tables else {}
    groups = {}
    for r in table.records:
        key = name_key(r["PFNA"], r["PLNA"])
        group = groups.setdefault(key, {"first_name": r["PFNA"], "last_name": r["PLNA"],
                                       "teams": {}, "values": {}, "copies": 0})
        group["copies"] += 1
        team = r.get("TGID", 0)
        group["teams"][team] = teams.get(team) or f"Team {team}"
        for f in fields:
            group["values"].setdefault(f["name"], set()).add(r[f["name"]])
    players = list(groups.values())
    for p in players:
        p["values"] = {k: sorted(v) for k, v in p["values"].items()}
        p["teams"] = [{"id": k, "name": v} for k, v in sorted(p["teams"].items())]
    return {"template_sha256": hashlib.sha256(buf).hexdigest(), "fields": fields, "players": players}


def write_export(buf: bytes, output: Path, entry: int = 2, as_csv: bool = False):
    """Export current values privately; these files are not shareable packs."""
    data = editable(buf, entry)
    output = local_output(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8", newline="") as stream:
        if not as_csv:
            json.dump(data, stream, indent=2)
        else:
            writer = csv.writer(stream)
            writer.writerow(["first_name", "last_name", "teams", *[f["name"] for f in data["fields"]]])
            for p in data["players"]:
                writer.writerow([p["first_name"], p["last_name"], "|".join(str(t["id"]) for t in p["teams"]),
                                 *["|".join(map(str, p["values"][f["name"]])) for f in data["fields"]]])


def main(argv=None) -> int:
    """Keep the reader CLI stable and add explicitly private writer outputs."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summary"); s.add_argument("file")
    d = sub.add_parser("dump"); d.add_argument("file"); d.add_argument("table")
    d.add_argument("--entry", type=int, help="TERF entry index (default: all)")
    d.add_argument("--csv", action="store_true")
    for command in ("apply", "export-editable"):
        p = sub.add_parser(command)
        p.add_argument("file")
        if command == "apply":
            p.add_argument("pack")
            p.add_argument("--base-sha256", help="SHA-256 of the source ISO, if pinned by pack")
        else:
            p.add_argument("--csv", action="store_true")
        p.add_argument("--entry", type=int, default=2)
        p.add_argument("--out", required=True, help="new output under local/")
    a = ap.parse_args(argv)
    if a.cmd in {"apply", "export-editable"}:
        try:
            buf = Path(a.file).read_bytes()
            if a.cmd == "export-editable":
                write_export(buf, Path(a.out), a.entry, a.csv)
            else:
                edits = read_pack(Path(a.pack).read_text(encoding="utf-8"))
                out, report = apply_edits(buf, edits, a.entry, a.base_sha256)
                dest = local_output(a.out)
                dest.parent.mkdir(parents=True, exist_ok=True)
                with dest.open("xb") as stream:
                    stream.write(out)
                print(json.dumps(report, indent=2))
        except (OSError, ValueError) as exc:
            print(str(exc), file=sys.stderr)
            return 1
        return 0

    entries = load(a.file)
    if a.cmd == "summary":
        non_db = [i for i, t in entries if t is None]
        dbs = [(i, t) for i, t in entries if t]
        print(f"{a.file}: {len(entries)} entr{'y' if len(entries) == 1 else 'ies'}, "
              f"{len(dbs)} DB, {len(non_db)} empty/non-DB")
        # Entries with the same table list (e.g. the 32 team DBs) are shown once.
        seen: dict[tuple, list[int]] = {}
        for i, tables in dbs:
            seen.setdefault(tuple(tables), []).append(i)
        for names, idxs in seen.items():
            tables = dict(dbs)[idxs[0]]
            label = f"entry {idxs[0]}" if len(idxs) == 1 else f"entries {idxs[0]}..{idxs[-1]} ({len(idxs)}, same tables; counts from entry {idxs[0]})"
            print(f" {label}:")
            for t in tables.values():
                fl = ", ".join(f"{f.name}:{f.type}/{f.bits}b" for f in sorted(t.fields, key=lambda f: f.bit_offset))
                print(f"  {t.name:5} {t.count:>5}/{t.capacity:<5} rec={t.record_bytes}B  {fl}")
        return 0

    rows = []
    for i, tables in entries:
        if tables is None or (a.entry is not None and i != a.entry) or a.table not in tables:
            continue
        rows += [{"entry": i, **r} for r in tables[a.table].records]
    if a.csv:
        if rows:
            w = csv.DictWriter(sys.stdout, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    else:
        print(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
