#!/usr/bin/env python3
"""Same-size GameCube FST file replacement into a new private ISO copy.

The input is read-only. Header, FST, DOL and every non-target byte retain their
positions. Refuse malformed/overlapping entries, stale expected bytes, existing
outputs and outputs outside local/. This module does not depend on isopatch.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eadb


class PatchError(ValueError):
    """Unsafe FST layouts and stale replacements fail before publication."""


def sha256(path: str | Path) -> str:
    """Hash a whole image with bounded memory, matching nsr_patcher's digest."""
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while chunk := stream.read(16 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def files(image: str | Path) -> dict[str, tuple[int, int]]:
    """Read a bounded GameCube FST, resolving its preorder directory hierarchy."""
    with open(image, "rb") as stream:
        image_size = os.fstat(stream.fileno()).st_size
        header = stream.read(0x440)
        if len(header) != 0x440 or header[0x1C:0x20] != bytes.fromhex("c2339f3d"):
            raise PatchError("not a GameCube disc image")
        offset, size = struct.unpack_from(">II", header, 0x424)
        if offset < 0x440 or size < 12 or size > 32 << 20 or offset + size > image_size:
            raise PatchError("invalid FST bounds")
        stream.seek(offset)
        fst = stream.read(size)
    kind, parent, count = struct.unpack_from(">III", fst)
    if kind != 0x01000000 or parent or not 1 <= count <= size // 12:
        raise PatchError("invalid FST root")
    strings = count * 12
    stack = [(0, count, "")]
    result, paths = {}, set()
    for i in range(1, count):
        while i >= stack[-1][1]:
            stack.pop()
        word, start, length = struct.unpack_from(">III", fst, i * 12)
        kind, name_offset = word >> 24, strings + (word & 0xFFFFFF)
        end = fst.find(b"\0", name_offset)
        if kind not in (0, 1) or not strings <= name_offset < size or end < 0:
            raise PatchError(f"invalid FST entry {i}")
        name = fst[name_offset:end].decode("latin-1")
        if not name or name in (".", "..") or "/" in name or "\\" in name:
            raise PatchError("unsafe FST path")
        path = stack[-1][2] + name
        if path in paths:
            raise PatchError(f"duplicate FST path: {path}")
        paths.add(path)
        if kind:
            if start != stack[-1][0] or not i < length <= stack[-1][1]:
                raise PatchError("invalid FST directory range/parent")
            stack.append((i, length, path + "/"))
        else:
            if start < offset + size or start + length > image_size:
                raise PatchError(f"invalid file extent: {path}")
            result[path] = (start, length)
    # Aliased file extents would make replacing one file change another as well.
    extents = sorted((o, o + n) for o, n in result.values() if n)
    if any(a[1] > b[0] for a, b in zip(extents, extents[1:])):
        raise PatchError("overlapping FST file extents")
    return result


def read_file(image: str | Path, name: str) -> bytes:
    """Read the exact FST path; basenames do not silently select nested files."""
    entries = files(image)
    if name not in entries:
        raise PatchError(f"FST file not found: {name}")
    offset, size = entries[name]
    if size > 64 << 20:
        raise PatchError("roster file exceeds the 64 MiB safety limit")
    with open(image, "rb") as stream:
        stream.seek(offset)
        data = stream.read(size)
    if len(data) != size:
        raise PatchError("truncated image file")
    return data


def patch_copy(source: str | Path, output: str | Path, name: str, replacement: bytes,
               *, expect: bytes, expect_image_sha256: str | None = None) -> str:
    """Expect-check, copy, patch and verify; atomically publish a fresh image.

    Return the full resulting image SHA-256. A hard-link publication is exclusive
    (never replaces an existing image), and the temporary file is in the same
    directory/filesystem. A failed build leaves no partial output.
    """
    source = Path(source).resolve()
    output = eadb.local_output(output)
    if output.exists() or output == source:
        raise PatchError("output must be a new image copy")
    original = read_file(source, name)
    if original != expect:
        raise PatchError("stale: expected disc file bytes differ")
    if len(replacement) != len(original):
        raise PatchError("replacement must have exactly the original file size")
    if expect_image_sha256 and sha256(source) != expect_image_sha256:
        raise PatchError("stale: source image SHA-256 differs")
    offset, size = files(source)[name]
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".ratings-", suffix=".iso", dir=output.parent)
    try:
        with os.fdopen(fd, "w+b") as target, source.open("rb") as src:
            # Hash what was copied too, catching source changes during the copy.
            digest = hashlib.sha256()
            while chunk := src.read(16 << 20):
                target.write(chunk)
                digest.update(chunk)
            if expect_image_sha256 and digest.hexdigest() != expect_image_sha256:
                raise PatchError("source image changed while copying")
            target.seek(offset)
            if target.read(size) != expect:
                raise PatchError("source file changed while copying")
            target.seek(offset)
            target.write(replacement)
            target.flush()
            os.fsync(target.fileno())
        if read_file(temporary, name) != replacement or os.path.getsize(temporary) != source.stat().st_size:
            raise PatchError("replacement verification failed")
        result = sha256(temporary)
        os.link(temporary, output)
        return result
    finally:
        os.unlink(temporary)


def _extent(image: str | Path, name: str, replacement: bytes) -> tuple[int, int]:
    entries = files(image)
    if name not in entries:
        raise PatchError(f"FST file not found: {name}")
    if len(replacement) != entries[name][1]:
        raise PatchError("replacement must have exactly the original file size")
    return entries[name]


def spliced_sha256(source: str | Path, name: str, replacement: bytes) -> str:
    """SHA-256 the image would have with FST file `name` replaced, without
    writing anything. The content publisher uses it to predict every friend's
    output fingerprint from the editor's own base image (docs/content-updates.md)."""
    offset, size = _extent(source, name, replacement)
    digest = hashlib.sha256()
    with open(source, "rb") as stream:
        remaining = offset
        while remaining:
            chunk = stream.read(min(remaining, 16 << 20))
            if not chunk:
                raise PatchError("truncated image file")
            digest.update(chunk)
            remaining -= len(chunk)
        digest.update(replacement)
        stream.seek(offset + size)
        while chunk := stream.read(16 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def replace_in_place(image: str | Path, name: str, replacement: bytes, *, expect_sha256: str) -> None:
    """Overwrite same-size FST file `name` inside a private, not-yet-published
    build file (the launcher's nsr-net.iso.partial). Unlike patch_copy this
    edits the given file, so callers must only pass their own temporary
    output, never a source image; the caller verifies the whole image digest
    afterwards. The current file bytes must hash to expect_sha256."""
    offset, size = _extent(image, name, replacement)
    with open(image, "r+b") as stream:
        stream.seek(offset)
        if hashlib.sha256(stream.read(size)).hexdigest() != expect_sha256:
            raise PatchError("stale: roster file differs from the expected bytes")
        stream.seek(offset)
        stream.write(replacement)
        stream.flush()
        os.fsync(stream.fileno())
    if read_file(image, name) != replacement:
        raise PatchError("replacement verification failed")


def main(argv=None) -> int:
    """Require an explicit expected file and a fresh private output copy."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image")
    parser.add_argument("file", help="exact path in disc FST, e.g. template.dat")
    parser.add_argument("replacement")
    parser.add_argument("--expect", required=True, help="original extracted file")
    parser.add_argument("--expect-image-sha256")
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    try:
        digest = patch_copy(args.image, args.out, args.file, Path(args.replacement).read_bytes(),
                            expect=Path(args.expect).read_bytes(),
                            expect_image_sha256=args.expect_image_sha256)
        print(f"Verified SHA-256: {digest}")
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
