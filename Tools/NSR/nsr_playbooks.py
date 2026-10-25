"""Signed account playbooks and frozen room bundles (references only, no game assets).

The Access editor owns publication. Launchers resolve immutable envelopes from the
existing content store and build one pad-ordered runtime bundle on every peer.
Native category indices remain bounded by the owner's unchanged play library.
"""
from __future__ import annotations

import hashlib
import re
import secrets

import nsr_content as content
import rsasig

FORMAT = 'nsrb.playbook.v1'
ENVELOPE = 'nsrb.playbook.signed.v1'
MAX_BYTES = 64 * 1024
VERSION = 1
HEX = re.compile(r'[0-9a-f]{64}')


def owner_id(code: str) -> str:
    """Derive the public launcher ID without exposing its private linking code."""
    if not isinstance(code, str) or not HEX.fullmatch(code):
        raise content.ContentError('Link code must be the 64 characters shown in your launcher.')
    return hashlib.sha256(('nsrb.playbooks.link.v1:' + code).encode()).hexdigest()


def new_code() -> str:
    """Create a private, unguessable pairing capability on this installation."""
    return secrets.token_hex(32)


def validate_pack(pack) -> dict:
    """Bound every field before publication, signature acceptance or native use."""
    content._keys(pack, 'playbook', {'format', 'owner', 'revision', 'name', 'base_sha256',
                                   'igdata_sha256', 'groups'})
    if pack['format'] != FORMAT:
        raise content.ContentError('Unsupported playbook format.')
    for key in ('owner', 'base_sha256', 'igdata_sha256'):
        content._hex64(pack[key], key)
    content._int(pack['revision'], 'revision', 1, content.MAX_VERSION)
    content._text(pack['name'], 'playbook name', 64)
    try:
        pack['name'].encode('utf-8')
    except UnicodeEncodeError:
        raise content.ContentError('Playbook name must be valid Unicode.') from None
    groups = pack['groups']
    if not isinstance(groups, list) or not 1 <= len(groups) <= 16:
        raise content.ContentError('Playbook requires 1..16 categories.')
    seen = set()
    for group in groups:
        content._keys(group, 'category', {'entry', 'pbst', 'library', 'slots'})
        if type(group['entry']) is not int or group['entry'] not in (1, 2):
            raise content.ContentError('Unknown offense/defense library.')
        content._int(group['pbst'], 'category ID', 0, 65535)
        key = (group['entry'], group['pbst'])
        if key in seen:
            raise content.ContentError('Duplicate category.')
        seen.add(key)
        for field in ('library', 'slots'):
            values = group[field]
            if not isinstance(values, list) or not 1 <= len(values) <= 127:
                raise content.ContentError('Each category needs 1..127 slots.')
            for value in values:
                content._int(value, 'play occurrence ID', 0, 1023)
            if len(set(values)) != len(values):
                raise content.ContentError('A category cannot contain duplicate plays.')
        if not set(group['slots']) <= set(group['library']):
            raise content.ContentError('Choose plays from this category’s library.')
    if [(g['entry'], g['pbst']) for g in groups] != sorted(seen):
        raise content.ContentError('Categories must be in canonical order.')
    return pack


def catalog(image: str) -> dict:
    """Read the owner's local game library for the private authenticated editor."""
    import discpatch
    import eadb
    import playbooks
    data = discpatch.read_file(image, 'igdata.dat')
    groups = []
    for index in (1, 2):
        tables = eadb.parse_db(playbooks.entry(data, index))
        for pbst in sorted({r['PBST'] for r in tables['PBPL'].records}):
            rows = sorted((r for r in tables['PBPL'].records if r['PBST'] == pbst),
                          key=lambda r: r['ord_'])
            # PBST category labels are private catalog presentation, never pack data.
            category = next((r for r in tables['PBST'].records if r['PBST'] == pbst), {})
            groups.append({'entry': index, 'pbst': pbst, 'name': category.get('name', str(pbst)),
                           'plays': [{'id': r['PBPL'], 'name': r['name']} for r in rows]})
    return {'igdata_sha256': hashlib.sha256(data).hexdigest(), 'groups': groups}


def validate_library(pack: dict, library: dict) -> None:
    """Reject stale bases, missing categories and references outside this image."""
    validate_pack(pack)
    expected = {(g['entry'], g['pbst']): [p['id'] for p in g['plays']] for g in library['groups']}
    if (pack['igdata_sha256'] != library['igdata_sha256'] or
            {(g['entry'], g['pbst']): g['library'] for g in pack['groups']} != expected):
        raise content.ContentError('Playbook library changed. Reload the editor and republish.')


def sign(pack: dict, signer) -> bytes:
    """Use the existing server signing key; no private key reaches a launcher."""
    body = content.canonical(validate_pack(pack))
    raw = content.canonical({'format': ENVELOPE, 'kid': signer.kid,
                             'payload': content.b64encode(body),
                             'sig': content.b64encode(signer.sign(body))})
    if len(raw) > MAX_BYTES:
        raise content.ContentError('Playbook pack is too large.')
    return raw


def verify(raw: bytes, keys=None) -> dict:
    """Recheck trusted signature and canonical schema, including cached packs."""
    if not isinstance(raw, bytes) or len(raw) > MAX_BYTES:
        raise content.ContentError('Playbook envelope is too large.')
    env = content._keys(content.strict_loads(raw), 'signed playbook', {'format', 'kid', 'payload', 'sig'})
    if env['format'] != ENVELOPE or not isinstance(env['kid'], str):
        raise content.ContentError('Unsupported signed playbook.')
    trusted = content.trusted_keys(keys)
    if env['kid'] not in trusted:
        raise content.ContentError('Playbook signer is not trusted by this download.')
    body, sig = content.b64decode(env['payload']), content.b64decode(env['sig'])
    n, e = trusted[env['kid']]
    if not rsasig.rsa_pkcs1v15_sha256_verify(n, e, body, sig):
        raise content.ContentError('Playbook signature is invalid.')
    pack = validate_pack(content.strict_loads(body))
    if content.canonical(pack) != body or content.canonical(env) != raw:
        raise content.ContentError('Playbook is not canonical JSON.')
    return pack


def descriptor(raw: bytes, pack: dict) -> dict:
    """Small public room reference; email and pairing capability stay private."""
    return {'owner': pack['owner'], 'revision': pack['revision'], 'name': pack['name'],
            'sha256': hashlib.sha256(raw).hexdigest()}


def parse_descriptor(value) -> dict | None:
    """Reject malformed room extensions instead of quietly selecting stock."""
    if value is None:
        return None
    content._keys(value, 'room playbook', {'owner', 'revision', 'name', 'sha256'})
    content._hex64(value['owner'], 'owner')
    content._hex64(value['sha256'], 'playbook digest')
    content._int(value['revision'], 'revision', 1, content.MAX_VERSION)
    content._text(value['name'], 'name', 64)
    try:
        value['name'].encode('utf-8')
    except UnicodeEncodeError:
        raise content.ContentError('Playbook name must be valid Unicode.') from None
    return dict(value)


def room_set(value, players: int | None = None) -> list:
    """Pad order and captain policy are part of the shared immutable identity."""
    if value is None:
        return []
    if not isinstance(value, list) or not 1 <= len(value) <= 4 or (players and len(value) != players):
        raise content.ContentError('Invalid room playbook roster.')
    return [parse_descriptor(v) for v in value]


def label(books: list) -> str:
    """Render the same book/revision labels in either launcher lobby view."""
    labels = []
    for i, book in enumerate(books):
        try:
            book = parse_descriptor(book)
            labels.append(f'P{i + 1}: {book["name"]} r{book["revision"]}' if book else f'P{i + 1}: Game default')
        except content.ContentError:
            labels.append(f'P{i + 1}: Invalid playbook — fetch it again')
    return ' · '.join(labels)


class Cache:
    """Immutable signed envelopes in the existing content-addressed pack cache."""

    def __init__(self, folder: str, keys=None, fetch=content.http_get):
        """Share storage and downloader conventions with rating content."""
        self.content = content.Cache(folder, keys)
        self.keys, self.fetch = keys, fetch

    def current(self, base: str, owner: str) -> dict:
        """Fetch the linked account's latest revision; failures remain explicit."""
        content._hex64(owner, 'owner')
        raw = self.fetch(base + '/playbooks/users/' + owner + '.json', MAX_BYTES)
        pack = verify(raw, self.keys)
        if pack['owner'] != owner:
            raise content.ContentError('Playbook belongs to another launcher account.')
        ref = descriptor(raw, pack)
        content._atomic_write(self.content._pack_path(ref['sha256']), raw)
        return ref

    def resolve(self, base: str, ref: dict) -> dict:
        """Recheck cache; recover corruption once using the same frozen digest."""
        ref = parse_descriptor(ref)
        path = self.content._pack_path(ref['sha256'])

        def checked(raw: bytes) -> dict:
            """A signature alone cannot substitute another account or revision."""
            if hashlib.sha256(raw).hexdigest() != ref['sha256']:
                raise content.ContentError('Playbook digest differs from the room.')
            pack = verify(raw, self.keys)
            if descriptor(raw, pack) != ref:
                raise content.ContentError('Signed playbook identity differs from the room.')
            return pack

        try:
            with open(path, 'rb') as stream:
                return checked(stream.read(MAX_BYTES + 1))
        except (OSError, content.ContentError):
            # Immutable packs can be safely fetched again, but a failed repair
            # must not choose a different revision or silently revert to stock.
            if not base:
                raise content.ContentError('A room playbook is not available offline. Connect and reopen the room.') from None
        try:
            raw = self.fetch(base + '/packs/' + ref['sha256'] + '.json', MAX_BYTES)
        except OSError:
            raise content.ContentError(f'Could not download room playbook “{ref["name"]}”. Check the connection and reopen the room.') from None
        pack = checked(raw)
        content._atomic_write(path, raw)
        return pack

    def runtime(self, base: str, refs: list, image: str) -> list:
        """Verify every peer's pack against the same local image before boot."""
        import discpatch
        refs = room_set(refs)
        if not any(refs):
            return []
        library, image_sha = catalog(image), discpatch.sha256(image)
        combined = []
        for pad, ref in enumerate(refs):
            if ref:
                pack = self.resolve(base, ref)
                if pack['base_sha256'] != image_sha:
                    raise content.ContentError('Custom playbooks need their exact published base image.')
                validate_library(pack, library)
                combined.append({'pad': pad, 'pack': pack})
            else:
                combined.append({'pad': pad, 'pack': None})
        return combined
