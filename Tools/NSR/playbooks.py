#!/usr/bin/env python3
"""Inspect NFL Street 2 playbooks and build an edits-only reorder proof.

GN7E69 rev 0 / Reborn: igdata.dat TERF slots 1 (defense), 2 (offense).
All extracted catalogs and built images must stay below local/. This prototype
preserves DB CRC words and changes existing PBPL ord_ fields; route and
assignment opcodes remain read-only until their semantics are qualified.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile

import discpatch
import eadb
import ealzh
import isopatch

# GN7E69 rev 0: the published order bypasses the saved USOP/USDP name-prefix
# ordering path. The native fallback enumerates the validated book ordinals.
ORDER_SITE = 0x80057D9C
ORDER_FALLBACK = 0x80057EC4
ORDER_EXPECT = 0x4BFFEAAD  # original bl fn_80056848


@dataclass(frozen=True)
class Slot:
    """Stored extent plus the metadata needed for same-size TERF replacement."""
    offset: int
    size: int
    capacity: int
    method: int
    decoded_size: int
    directory_offset: int


def slots(data: bytes) -> list[Slot]:
    """Read the aligned TERF/DIR1/COMP/DATA variant used by igdata.dat."""
    if len(data) < 0x40 or data[:4] != b'TERF':
        raise ValueError('not TERF')
    alignment = struct.unpack_from('>I', data, 4)[0]
    count = struct.unpack_from('>H', data, 14)[0]
    if alignment != 0x800 or not 1 <= count <= 128:
        raise ValueError('unsupported igdata TERF header')
    directory, comp, payload = alignment, alignment * 2, alignment * 3
    for start, tag in [(directory, b'DIR1'), (comp, b'COMP'), (payload, b'DATA')]:
        if data[start:start + 4] != tag or start + 8 + count * 8 > len(data):
            raise ValueError('unsupported TERF chunk layout')
    if struct.unpack_from('>I', data, payload + 4)[0] != len(data) - payload:
        raise ValueError('DATA chunk extent differs')
    entries = [struct.unpack_from('>II', data, directory + 8 + i * 8) for i in range(count)]
    result = []
    for i, (offset, size) in enumerate(entries):
        end = entries[i + 1][0] if i + 1 < count else len(data) - payload
        if offset < alignment or offset % alignment or not 0 < size <= end - offset:
            raise ValueError('invalid TERF slot extent')
        method, decoded_size = struct.unpack_from('>II', data, comp + 8 + i * 8)
        if method not in (0, 5) or not 0 < decoded_size <= ealzh.MAX_OUTPUT:
            raise ValueError('unsupported TERF compression')
        result.append(Slot(payload + offset, size, end - offset, method, decoded_size,
                           directory + 8 + i * 8))
    return result


def entry(data: bytes, index: int) -> bytes:
    """Decode one bounded slot, retaining all schema descriptors in the DB."""
    found = slots(data)
    if type(index) is not int or not 0 <= index < len(found):
        raise ValueError('invalid TERF entry')
    slot = found[index]
    raw = data[slot.offset:slot.offset + slot.size]
    if slot.method == 5:
        return ealzh.decompress(raw, slot.decoded_size)
    if len(raw) != slot.decoded_size:
        raise ValueError('uncompressed size differs')
    return raw


def replace(data: bytes, index: int, decoded: bytes) -> bytes:
    """Recompress within an existing aligned slot; never move another asset."""
    slot = slots(data)[index]
    if len(decoded) != slot.decoded_size:
        raise ValueError('prototype requires unchanged decoded size')
    raw = ealzh.compress(decoded) if slot.method == 5 else decoded
    if len(raw) > slot.capacity:
        raise ValueError('edited entry exceeds existing aligned slot capacity')
    out = bytearray(data)
    out[slot.offset:slot.offset + max(slot.size, len(raw))] = raw.ljust(max(slot.size, len(raw)), b'\0')
    struct.pack_into('>I', out, slot.directory_offset + 4, len(raw))
    result = bytes(out)
    if entry(result, index) != decoded or len(result) != len(data):
        raise ValueError('TERF replacement verification failed')
    return result


def reorder(database: bytes, groups: list[dict]) -> bytes:
    """Validate exact PBPL permutations, then update only packed ord_ fields.

    PBPL identities and their AI weights stay attached to their original plays.
    A base hash in the enclosing pack binds expected IDs to a specific image's
    igdata; no copied names, instruction arrays, or play-art assets are shipped.
    """
    tables = eadb.parse_db(database)
    table = tables['PBPL']
    field = next(f for f in table.fields if f.name == 'ord_')
    if field.type != 3:
        raise ValueError('unsupported ordinal field')
    actions, seen = [], set()
    if not isinstance(groups, list) or not groups:
        raise ValueError('nonempty reorder groups required')
    for group in groups:
        if not isinstance(group, dict) or set(group) != {'pbst', 'pbpl'}:
            raise ValueError('reorder group needs pbst and pbpl')
        category, identities = group['pbst'], group['pbpl']
        if type(category) is not int or category in seen:
            raise ValueError('invalid/duplicate category')
        seen.add(category)
        rows = {r['PBPL']: i for i, r in enumerate(table.records) if r['PBST'] == category}
        if (not isinstance(identities, list) or not identities or
                any(type(i) is not int for i in identities) or
                len(set(identities)) != len(identities) or set(identities) != set(rows)):
            raise ValueError('each category must be an exact permutation of existing PBPL IDs')
        if len(identities) >= 1 << field.bits:
            raise ValueError('ordinal field capacity exceeded')
        actions.extend((rows[identity], ordinal) for ordinal, identity in enumerate(identities, 1))
    output = bytearray(database)
    shift = table.record_bytes * 8 - field.bit_offset - field.bits
    mask = ((1 << field.bits) - 1) << shift
    for row, value in actions:
        start = table.records_offset + row * table.record_bytes
        raw = int.from_bytes(output[start:start + table.record_bytes], 'big')
        output[start:start + table.record_bytes] = ((raw & ~mask) | (value << shift)).to_bytes(table.record_bytes, 'big')
    return bytes(output)


def apply_pack(data: bytes, pack: dict) -> bytes:
    """Bind the proof format to exact source bytes and offense/defense entries."""
    if (not isinstance(pack, dict) or set(pack) != {'schema', 'igdata_sha256', 'books'} or
            pack['schema'] != 'nsrb.playbook-order.v1' or
            pack['igdata_sha256'] != hashlib.sha256(data).hexdigest()):
        raise ValueError('unsupported pack or stale igdata hash')
    books = pack['books']
    if not isinstance(books, list) or not 1 <= len(books) <= 2:
        raise ValueError('one or two books required')
    output, seen = data, set()
    for book in books:
        if not isinstance(book, dict) or set(book) != {'entry', 'groups'}:
            raise ValueError('book needs entry and groups')
        index = book['entry']
        if type(index) is not int or index not in (1, 2) or index in seen:
            raise ValueError('unique offense/defense entry required')
        seen.add(index)
        output = replace(output, index, reorder(entry(data, index), book['groups']))
    return output


def catalog(data: bytes) -> dict:
    """Private export of format metadata and rows for local research/UI input."""
    result = {'igdata_sha256': hashlib.sha256(data).hexdigest(), 'entries': {}}
    for index in (1, 2):
        tables = eadb.parse_db(entry(data, index))
        result['entries'][index] = {name: {'stride': t.record_bytes, 'capacity': t.capacity,
            'fields': [vars(f) for f in t.fields], 'rows': t.records} for name, t in tables.items()}
    return result


def demo_pack(data: bytes) -> dict:
    """Derive a first/last swap recipe locally, without shipping base game rows."""
    books = []
    for index in (1, 2):
        rows = eadb.parse_db(entry(data, index))['PBPL'].records
        groups = []
        for category in sorted({row['PBST'] for row in rows}):
            members = sorted((row for row in rows if row['PBST'] == category),
                             key=lambda row: row['ord_'])
            if len(members) < 2:
                continue
            identities = [row['PBPL'] for row in members]
            identities[0], identities[-1] = identities[-1], identities[0]
            groups.append({'pbst': category, 'pbpl': identities})
        if groups:
            books.append({'entry': index, 'groups': groups})
    if not books:
        raise ValueError('no populated categories for a reorder proof')
    return {'schema': 'nsrb.playbook-order.v1',
            'igdata_sha256': hashlib.sha256(data).hexdigest(), 'books': books}


def patch_qkl(bundle: bytes, before: bytes, after: bytes) -> bytes:
    """Update cached igdata header/entries through QL01 FILS/DTLS records.

    Match bundles store a TERF header and selected compressed entries at
    unrelated offsets. Replacing only the loose DAT silently leaves gameplay
    unchanged. Preserve every DTLS offset and require exact source copies.
    """
    if bundle[:8] != b'QL01\0\0\0\x0c' or len(bundle) < 24:
        raise ValueError('unsupported QKL header')
    payload = struct.unpack_from('>I', bundle, 8)[0]
    if bundle[12:16] != b'FILS':
        raise ValueError('missing QKL file list')
    size, count = struct.unpack_from('>II', bundle, 16)
    if size != 12 + 48 * count or 12 + size > len(bundle):
        raise ValueError('invalid QKL file list')
    names = [bundle[24 + i * 48:24 + (i + 1) * 48].split(b'\0')[0]
             for i in range(count)]
    if names.count(b'igdata.dat') != 1:
        raise ValueError('QKL must contain one igdata.dat')
    file_index = names.index(b'igdata.dat')
    dtls = 12 + size
    if bundle[dtls:dtls + 4] != b'DTLS' or dtls + 12 > len(bundle):
        raise ValueError('missing QKL detail list')
    detail_size, detail_count = struct.unpack_from('>II', bundle, dtls + 4)
    if (detail_size != 12 + detail_count * 12 or dtls + detail_size != payload or
            bundle[payload:payload + 4] != b'DATA'):
        raise ValueError('invalid QKL detail extent')
    records = [struct.unpack_from('>HHII', bundle, dtls + 12 + i * 12)
               for i in range(detail_count)]
    offsets = sorted({payload + record[3] for record in records} | {len(bundle)})
    if not offsets or offsets[0] < payload + 8 or offsets[-1] != len(bundle):
        raise ValueError('invalid QKL payload offsets')
    output, seen = bytearray(bundle), set()
    old_slots, new_slots = slots(before), slots(after)
    changed = {i for i, (old, new) in enumerate(zip(old_slots, new_slots))
               if entry(before, i) != entry(after, i)}
    for kind, index, slot_index, relative in records:
        if index != file_index or kind not in (0, 1):
            continue
        key = (kind, slot_index)
        if key in seen:
            raise ValueError('duplicate QKL igdata detail')
        seen.add(key)
        start = payload + relative
        end = next(offset for offset in offsets if offset > start)
        if kind == 0:
            old, new = before[:0x1800], after[:0x1800]
        elif slot_index in changed:
            old_slot, new_slot = old_slots[slot_index], new_slots[slot_index]
            old = before[old_slot.offset:old_slot.offset + old_slot.size]
            new = after[new_slot.offset:new_slot.offset + new_slot.size]
        else:
            continue
        if sum(record[3] == relative for record in records) != 1:
            raise ValueError('cached igdata extent aliases another QKL resource')
        if max(len(old), len(new)) > end - start or bundle[start:start + len(old)] != old:
            raise ValueError('stale QKL copy or insufficient cached slot space')
        output[start:start + max(len(old), len(new))] = new.ljust(max(len(old), len(new)), b'\0')
    if (0, 0) not in seen or any((1, i) not in seen for i in changed):
        raise ValueError('QKL is missing a changed igdata entry')
    return bytes(output)


def build_image(source: str, destination: str, pack: dict) -> str:
    """Validate every cached copy, then publish one fully patched private ISO."""
    output = eadb.local_output(destination)
    if output.exists() or Path(source).resolve() == output:
        raise ValueError('output must be a new private image')
    before = discpatch.read_file(source, 'igdata.dat')
    after = apply_pack(before, pack)
    cached = []
    fst = discpatch.files(source)
    for index in range(20):
        name = f'game{index}.qkl'
        if name not in fst:
            raise ValueError(f'missing match bundle: {name}')
        old = discpatch.read_file(source, name)
        cached.append((name, hashlib.sha256(old).hexdigest(), patch_qkl(old, before, after)))
    output.parent.mkdir(parents=True, exist_ok=True)
    # Only our temporary copy is mutated; the final name is published exclusively
    # once all 21 file replacements and read-back verification have succeeded.
    with tempfile.TemporaryDirectory(prefix='.playbooks-', dir=output.parent) as folder:
        temporary = Path(folder) / 'build.iso'
        discpatch.patch_copy(source, temporary, 'igdata.dat', after, expect=before)
        for name, expected, replacement in cached:
            discpatch.replace_in_place(temporary, name, replacement, expect_sha256=expected)
        with temporary.open('r+b') as image:
            isopatch.write_words(image, ORDER_SITE,
                                 [isopatch.b(ORDER_SITE, ORDER_FALLBACK)], [ORDER_EXPECT])
        digest = discpatch.sha256(temporary)
        os.link(temporary, output)
    return digest


def main() -> int:
    """Keep image builds in the existing source-read-only discpatch workflow."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    inspect = sub.add_parser('inspect')
    inspect.add_argument('image')
    inspect.add_argument('output', help='private catalog JSON under local/')
    demo = sub.add_parser('demo-pack')
    demo.add_argument('image')
    demo.add_argument('output', help='private first/last swap pack under local/')
    build = sub.add_parser('build')
    build.add_argument('image')
    build.add_argument('pack')
    build.add_argument('output', help='new ISO under local/')
    args = parser.parse_args()
    with open(args.image, 'rb') as stream:
        header = stream.read(8)
    if len(header) != 8 or header[:6] != b'GN7E69' or header[7] != 0:
        raise ValueError('only GN7E69 revision 0 is supported')
    data = discpatch.read_file(args.image, 'igdata.dat')
    if args.command in ('inspect', 'demo-pack'):
        output = eadb.local_output(args.output)
        value = catalog(data) if args.command == 'inspect' else demo_pack(data)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('x') as stream:
            json.dump(value, stream, indent=2)
    else:
        # Strict duplicate-key and nonfinite-number rejection already exists in
        # the content module; use the same JSON parser for future integration.
        import nsr_content
        pack = nsr_content.strict_loads(Path(args.pack).read_bytes())
        print(build_image(args.image, args.output, pack))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
