"""Shared, stdlib-only roster-pack policy; artwork never enters guest identity.

Native field limits target GN7E69 revision 0. The editor additionally decodes and
re-encodes every uploaded PNG with Pillow before this signed-data boundary.
"""
import base64
import binascii
import re
import struct
import zlib

PACK_FORMAT = "nsrb.ratings-pack.v2"
RUNTIME_FORMAT = "nsrb.runtime-content.v5"
NAME_LIMITS = {"PFNA": 10, "PLNA": 13}
ROLES = {0: "QB", 1: "RB", 2: "WR", 11: "DB", 4: "OL", 9: "LB", 7: "DL"}
MAX_PNG = 256 * 1024
TEXTURE = re.compile(r"tex1_[1-9][0-9]*x[1-9][0-9]*(?:_m)?_[0-9a-f]{16}(?:_[0-9a-f]{16})?_(?:[0-9]|1[0-4])\Z")


def name(value, limit):
    """Require printable, trimmed CP1252 text with space for the native NUL."""
    if not isinstance(value, str) or value != value.strip() or "|" in value:
        raise ValueError("names must be trimmed CP1252 text without pipes")
    try:
        raw = value.encode("cp1252")
    except UnicodeEncodeError:
        raise ValueError("names must use Windows-1252 characters") from None
    if len(raw) > limit or any(b < 32 or b == 127 for b in raw):
        raise ValueError(f"name exceeds {limit} CP1252 bytes or contains controls")
    return value


def png_bytes(encoded):
    """Check canonical bounded PNG bytes without adding Pillow to the launcher.

    Restrict to the editor's normalized 256px RGBA output. CRC/chunk/inflate
    checks catch corrupt signed caches before writing any texture overlay.
    """
    if not isinstance(encoded, str) or len(encoded) > (MAX_PNG + 2) // 3 * 4:
        raise ValueError("headshot exceeds 256 KiB")
    try:
        raw = base64.b64decode(encoded, validate=True)
    except (ValueError, binascii.Error):
        raise ValueError("invalid headshot encoding") from None
    if (not raw.startswith(b"\x89PNG\r\n\x1a\n") or len(raw) > MAX_PNG or
            base64.b64encode(raw).decode("ascii") != encoded):
        raise ValueError("invalid headshot PNG")
    pos, chunks, pixels = 8, [], bytearray()
    while pos + 12 <= len(raw):
        size, tag = struct.unpack_from(">I4s", raw, pos)
        end = pos + 12 + size
        if end > len(raw) or tag not in (b"IHDR", b"IDAT", b"IEND"):
            raise ValueError("headshot contains invalid chunks or metadata")
        payload = raw[pos + 8:pos + 8 + size]
        if zlib.crc32(tag + payload) != struct.unpack_from(">I", raw, end - 4)[0]:
            raise ValueError("headshot PNG checksum differs")
        if tag == b"IHDR" and (chunks or payload != struct.pack(">IIBBBBB", 256, 256, 8, 6, 0, 0, 0)):
            raise ValueError("headshot must be normalized 256px RGBA")
        if tag == b"IDAT":
            pixels.extend(payload)
        chunks.append(tag)
        pos = end
        if tag == b"IEND":
            if size or pos != len(raw):
                raise ValueError("invalid headshot end")
            break
    if not chunks or chunks[0] != b"IHDR" or chunks[-1] != b"IEND" or not pixels:
        raise ValueError("incomplete headshot PNG")
    try:
        decoder = zlib.decompressobj()
        scan = decoder.decompress(pixels, 256 * (256 * 4 + 1) + 1)
        if (len(scan) != 256 * 1025 or not decoder.eof or decoder.unused_data or
                any(scan[i * 1025] > 4 for i in range(256))):
            raise ValueError("invalid headshot pixels")
    except zlib.error:
        raise ValueError("invalid headshot compression") from None
    return raw


def validate_headshots(items):
    """Reject ambiguous player/texture assignments in signed art packs."""
    if not isinstance(items, list) or len(items) > 100:
        raise ValueError("headshots must list at most 100 players")
    seen, textures = set(), set()
    for item in items:
        if not isinstance(item, dict) or set(item) != {"first_name", "last_name", "textures", "png"}:
            raise ValueError("invalid headshot keys")
        first, last = name(item["first_name"], 32), name(item["last_name"], 32)
        key = first.casefold(), last.casefold()
        if not (first or last) or key in seen:
            raise ValueError("duplicate or empty headshot player")
        seen.add(key)
        stems = item["textures"]
        if (not isinstance(stems, list) or not 1 <= len(stems) <= 100 or
                any(not isinstance(s, str) or not TEXTURE.fullmatch(s) for s in stems) or
                len(set(stems)) != len(stems) or textures.intersection(stems)):
            raise ValueError("invalid or conflicting headshot textures")
        textures.update(stems)
        png_bytes(item["png"])
    return items
