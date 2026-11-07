"""Signed application updates: the manifest format and the client's plan.

This is the half of the auto-updating launcher that can be reasoned about
without a network or a Windows desktop: what a release manifest may say, and
what a client should do about it. Downloading, staging and the self-replace
live in the launcher; they consume `plan()` and `verify_staged()` from here.

Why it reuses `nsr_content`: that module is already a working signed
distribution system with a trust anchor compiled into the launcher
(`TRUSTED_KEYS`), an RSA-3072 PKCS#1 v1.5 envelope whose verifier needs
nothing outside the standard library, serial-based replay protection and the
discipline that a failure keeps the working install. A second signing scheme
would be a second thing to get wrong, so this module brings only a payload
schema and shares the envelope, the key handling and the size limit.
`docs/release-plan.md` sketched Ed25519 for binary updates; that is the one
deliberate deviation, for the same reason its own content section gives -
staying inside the standard library - and so that one key, one anchor and one
revocation story cover both kinds of update.

Domain separation: the signed payload carries `format` = "nsrb.build.v1",
which the content validator rejects and this one requires, so a content
manifest can never be replayed as an application update or the reverse.

Ordering is by the integer `version`, never by the human label: a label is for
the player and a tag is a string, and string comparison on versions is a
classic source of downgrade bugs. A client applies a manifest only when its
version is strictly greater than what is installed, so a replayed older
manifest is inert even though its signature is still good.
"""
from __future__ import annotations

import hashlib
import os
import re

import nsr_content as content
from nsr_content import ContentError, canonical, _hex64, _int, _keys, _stamp, _text

MANIFEST_FORMAT = "nsrb.build.v1"

# sha256 of nothing. A package legitimately contains empty files - Dolphin's
# `Sys/Load/GraphicMods` has 36 placeholder .txt files - and they are a special
# case twice over: a release host will not store a 0-byte asset (GitHub returns
# "Bad Content-Length"), and there is nothing to transfer anyway, since the
# content is entirely determined by the size. So they are never uploaded and
# the client writes them.
EMPTY_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

# What this launcher understands, compared against a manifest's `min_launcher`.
# Raise it only when the install steps or manifest fields actually change: a
# build stamped above a friend's launcher format is refused outright, which
# strands them with no way forward but a fresh download.
LAUNCHER_FORMAT = 5

# Limits. The shipped package is ~92.6 MB across roughly 2700 files - Dolphin's
# `Sys/` alone is 2506 of them - so these are sized from a real package rather
# than from the "few dozen" the design sketch assumed, and still small enough
# that a hostile or broken release cannot make a launcher enumerate or hold
# much before anything is verified.
MAX_FILES = 8192

# An application manifest lists every shipped file, so it does not fit a
# content manifest's 64 KiB: ~2700 entries of ~148 bytes is about 390 KiB.
MAX_MANIFEST_BYTES = 2 * 1024 * 1024
MAX_FILE_BYTES = 256 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
MAX_REVOKED = 64
MAX_LABEL = 64
MAX_PATH = 160
MAX_DEPTH = 6

# A shipped path is relative, forward-slashed and boring. Windows is the target,
# so the rules are Windows' rules: no drive letters, no backslashes, no leading
# separator, no "." or ".." element, no trailing dot or space on any element
# (Windows silently strips them, which would let two entries land on one file),
# and no reserved device name.
#
# This is a blocklist of what Windows actually forbids, not an allowlist of
# what seemed likely. The allowlist here was guesswork and it was wrong: it
# rejected 359 files of the first real package, every one of them a legitimate
# Dolphin theme asset named `...@2x.png`, plus an apostrophe elsewhere. Both
# are perfectly legal filenames. Guessing the permitted set means rediscovering
# it one refused release at a time, so the rule now names the characters
# Windows reserves - <>:"/\|?* and the control range - and allows the rest of
# printable ASCII. Non-ASCII is still refused: it is not needed here, and two
# Unicode spellings of one name could normalize onto a single file.
_FORBIDDEN = set(r'<>:"/\|?*') | {chr(c) for c in range(32)} | {chr(127)}
_LABEL = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.+-]{0,63}")
_RESERVED = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)),
             *(f"lpt{i}" for i in range(1, 10))}
_COMMIT = re.compile(r"[0-9a-f]{40}")


def _valid_element(part: str) -> bool:
    """One path element: printable ASCII, nothing Windows reserves."""
    if not 1 <= len(part) <= 64 or not part.isascii():
        return False
    return not any(ch in _FORBIDDEN for ch in part)


def valid_path(value) -> str:
    """Return a shipped relative path, or raise. See the rules above."""
    if not isinstance(value, str) or not 1 <= len(value) <= MAX_PATH:
        raise ContentError("manifest file path is malformed")
    if "\\" in value or value.startswith("/") or ":" in value:
        raise ContentError(f"manifest file path is not relative: {value!r}")
    parts = value.split("/")
    if not 1 <= len(parts) <= MAX_DEPTH:
        raise ContentError(f"manifest file path is too deep: {value!r}")
    for part in parts:
        if not _valid_element(part) or part.strip(".") == "":
            raise ContentError(f"manifest file path is malformed: {value!r}")
        if part != part.rstrip(". "):
            raise ContentError(f"manifest file path element is unsafe: {value!r}")
        if part.split(".")[0].lower() in _RESERVED:
            raise ContentError(f"manifest file path is a reserved name: {value!r}")
    return value


def validate_build(obj) -> dict:
    """The release body: what it is, what it needs, and every file it ships."""
    _keys(obj, "build", {"version", "label", "published", "changelog", "min_launcher",
                         "fork_commit", "launcher_commit", "recipe", "room_min_version",
                         "revoked", "entry", "files"})
    _int(obj["version"], "build version", 1, content.MAX_VERSION)
    if not isinstance(obj["label"], str) or not _LABEL.fullmatch(obj["label"]):
        raise ContentError("build label is malformed")
    _stamp(obj["published"], "build published")
    _text(obj["changelog"], "changelog", content.MAX_CHANGELOG)
    _int(obj["min_launcher"], "min_launcher", 1, 1000)
    for name in ("fork_commit", "launcher_commit"):
        if not isinstance(obj[name], str) or not _COMMIT.fullmatch(obj[name]):
            raise ContentError(f"build {name} is malformed")
    if not isinstance(obj["recipe"], str) or not content._RECIPE.fullmatch(obj["recipe"]):
        raise ContentError("build recipe is malformed")
    # A room floor above this build would refuse its own release.
    _int(obj["room_min_version"], "room_min_version", 1, obj["version"])
    revoked = obj["revoked"]
    if not isinstance(revoked, list) or len(revoked) > MAX_REVOKED:
        raise ContentError(f"build must revoke at most {MAX_REVOKED} versions")
    for i, v in enumerate(revoked, 1):
        _int(v, f"revoked entry {i}", 1, content.MAX_VERSION)
    if len(set(revoked)) != len(revoked):
        raise ContentError("build revokes a version twice")
    if obj["version"] in revoked:
        raise ContentError("build revokes itself")

    files = obj["files"]
    if not isinstance(files, list) or not 1 <= len(files) <= MAX_FILES:
        raise ContentError(f"build must list 1..{MAX_FILES} files")
    total, seen = 0, {}
    for i, f in enumerate(files, 1):
        _keys(f, f"file entry {i}", {"path", "size", "sha256"})
        path = valid_path(f["path"])
        _hex64(f["sha256"], f"file entry {i} sha256")
        total += _int(f["size"], f"file entry {i} size", 0, MAX_FILE_BYTES)
        # An empty file's hash is not a free choice, and the client creates it
        # rather than downloading it (nothing to transfer), so a manifest that
        # paired size 0 with some other digest would describe a file no client
        # could ever produce.
        if f["size"] == 0 and f["sha256"] != EMPTY_SHA256:
            raise ContentError(f"file entry {i} is empty but does not have the empty digest")
        # Windows paths are case-insensitive, so two entries differing only in
        # case are one file there: a release like that is ambiguous, not clever.
        folded = path.lower()
        if folded in seen:
            raise ContentError(f"build lists {path!r} and {seen[folded]!r} as separate files")
        seen[folded] = path
    if total > MAX_TOTAL_BYTES:
        raise ContentError("build is larger than this launcher will download")
    if valid_path(obj["entry"]).lower() not in seen:
        raise ContentError("build entry point is not one of its files")
    return obj


def validate_payload(obj) -> dict:
    """The signed payload: envelope-level identity plus the build body."""
    _keys(obj, "manifest", {"format", "channel", "serial", "issued", "build"})
    if obj["format"] != MANIFEST_FORMAT:
        raise ContentError("unknown application manifest format")
    if obj["channel"] != content.CHANNEL:
        raise ContentError("manifest is for another channel")
    _int(obj["serial"], "manifest serial", 1, content.MAX_SERIAL)
    _stamp(obj["issued"], "manifest issued")
    validate_build(obj["build"])
    return obj


def sign(payload: dict, kid: str, sign_bytes) -> bytes:
    """Signed envelope for an application manifest (publisher side)."""
    return content.sign_manifest(payload, kid, sign_bytes, validate=validate_payload,
                                 max_bytes=MAX_MANIFEST_BYTES)


def verify(raw: bytes, keys=None) -> dict:
    """Payload of a signed application manifest, or raise. Client side."""
    return content.verify_manifest(raw, keys=keys, validate=validate_payload,
                                   max_bytes=MAX_MANIFEST_BYTES)


def describe(build: dict) -> str:
    """'update 2026.11.01 — <first changelog line>' for the launcher.

    The trailing full stop is dropped: callers embed this mid-sentence
    ("Installed {what}. Restart ...") and a changelog line that ends in one -
    most do - rendered as "Automatic updates.. Restart".
    """
    first = build["changelog"].strip().splitlines()[0][:120] if build["changelog"].strip() else ""
    first = first.rstrip().rstrip(".")
    return f"update {build['label']}" + (f" — {first}" if first else "")


def file_sha256(path: str, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while block := fh.read(chunk):
            h.update(block)
    return h.hexdigest()


def inventory(root: str, only: set[str] | None = None) -> dict[str, tuple[int, str]]:
    """{relative path: (size, sha256)} for an installed build.

    Paths that could not appear in a manifest are skipped rather than hashed,
    so a stray file with an odd name costs nothing and can never be mistaken
    for something the manifest covers.

    `only` restricts hashing to a known set of relative paths, and a real
    install needs it. The launcher's own folder is also where the player's
    `nsr-net.iso` (~1.4 GB), `User/`, `Assets/` and texture packs live, and
    those are valid-looking paths, so an unrestricted walk would hash the
    game image on every startup check just to decide whether the *launcher*
    changed. Pass the manifest's paths: nothing outside them affects the
    decision, and an updater has no business reading the player's game data.
    `stale` in `plan()` is therefore only meaningful on an unrestricted
    inventory - and even then it is something to report, never to delete,
    because that is exactly where mutable player data shows up.
    """
    out = {}
    for folder, _dirs, names in os.walk(root):
        for name in names:
            full = os.path.join(folder, name)
            if os.path.islink(full):
                continue
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if only is not None and rel not in only:
                continue
            try:
                valid_path(rel)
            except ContentError:
                continue
            out[rel] = (os.path.getsize(full), file_sha256(full))
    return out


class Plan:
    """What a client should do about a manifest it has verified.

    `blocked` is the one field to read first: when it is set, nothing should be
    downloaded or applied and the text is the plain-language reason. A blocked
    plan is still safe to act on - it just does nothing - which is what keeps
    an unreachable or hostile release from stopping play.
    """

    def __init__(self, *, build=None, fetch=(), reuse=(), stale=(), bytes_to_fetch=0,
                 rebuild_image=False, blocked="", revoked_installed=False):
        self.build = build
        self.fetch = list(fetch)                  # paths to download
        self.reuse = list(reuse)                  # already correct locally
        self.stale = list(stale)                  # installed, not in the manifest
        self.bytes_to_fetch = bytes_to_fetch
        self.rebuild_image = rebuild_image
        self.blocked = blocked
        self.revoked_installed = revoked_installed

    @property
    def up_to_date(self) -> bool:
        return not self.blocked and not self.fetch and self.build is not None

    def __repr__(self) -> str:
        if self.blocked:
            return f"Plan(blocked={self.blocked!r})"
        return (f"Plan(fetch={len(self.fetch)}, reuse={len(self.reuse)}, "
                f"stale={len(self.stale)}, bytes={self.bytes_to_fetch}, "
                f"rebuild_image={self.rebuild_image})")


def plan(payload: dict, *, installed_version: int, installed_recipe: str,
         have: dict[str, tuple[int, str]], launcher_format: int,
         last_serial: int = 0) -> Plan:
    """Decide what to do, given a verified manifest and what is on disk.

    Refusals come before anything else, because every one of them has to leave
    the player on the build they already have:

    * a replayed or reused serial, which is how a stale manifest is held back;
    * a version at or below what is installed, so a signed older manifest can
      never walk a player backwards;
    * a build that needs a newer launcher than this one, which may use fields
      or steps this code does not know about.

    A revoked *installed* version is reported rather than refused: it is the
    one case where staying put is also wrong, so the caller is told so it can
    insist on the update and refuse rooms until it applies.
    """
    build = payload["build"]
    revoked_installed = installed_version in build["revoked"]
    if payload["serial"] <= last_serial:
        return Plan(blocked="this update was already offered",
                    revoked_installed=revoked_installed)
    if build["version"] <= installed_version:
        return Plan(blocked="this download is already up to date",
                    revoked_installed=revoked_installed)
    if build["min_launcher"] > launcher_format:
        return Plan(blocked="this update needs a newer NFLStreetRemastered download",
                    revoked_installed=revoked_installed)

    fetch, reuse, total = [], [], 0
    for f in build["files"]:
        current = have.get(f["path"])
        if current == (f["size"], f["sha256"]):
            reuse.append(f["path"])
        else:
            fetch.append(f["path"])
            total += f["size"]
    wanted = {f["path"].lower() for f in build["files"]}
    stale = sorted(p for p in have if p.lower() not in wanted)
    return Plan(build=build, fetch=fetch, reuse=reuse, stale=stale, bytes_to_fetch=total,
                rebuild_image=build["recipe"] != installed_recipe,
                revoked_installed=revoked_installed)


def verify_staged(payload: dict, staging: str, *, only=None) -> list[str]:
    """Check staged downloads against the manifest; return the bad paths.

    Every file the manifest lists is checked by size and hash, not just the
    ones that were downloaded, so a half-applied or locally corrupted staging
    directory cannot pass. `only` is a partial diagnostic check, not proof of
    a complete stage: neither the wire hash nor an earlier install/inventory
    hash proves a file is still present and unchanged. Stage and swap therefore
    both check the whole manifest, including hard-linked/copied files.
    """
    bad = []
    for f in payload["build"]["files"]:
        if only is not None and f["path"] not in only:
            continue
        full = os.path.join(staging, *f["path"].split("/"))
        try:
            if os.path.getsize(full) != f["size"] or file_sha256(full) != f["sha256"]:
                bad.append(f["path"])
        except OSError:
            bad.append(f["path"])
    return bad


# Actions a refusal can carry, so the launcher's UI knows whose problem it is.
# Unlike a content update, neither can be retried inside the join: applying a
# build means replacing the running executable and relaunching, so the only
# honest instruction is to restart the launcher.
BUILD_UPDATE_ACTION = "update_build"            # the joiner is behind
HOST_BUILD_UPDATE_ACTION = "host_update_build"  # the host is behind


def build_descriptor(version: int, room_min: int) -> dict:
    """What a peer tells a room about its build: {version, room_min}."""
    return {"version": int(version), "room_min": int(room_min)}


def parse_build_descriptor(value) -> dict | None:
    """A peer's build descriptor, or None when it is absent or unusable.

    None means "do not compare", which is how a launcher from before build
    updates is admitted on the strength of the other checks. A malformed
    descriptor is treated the same way rather than refused: the fingerprint,
    protocol and content checks still stand between it and a desynced game,
    and refusing a room over an unparseable optional field would be a worse
    failure than ignoring it.
    """
    if not isinstance(value, dict):
        return None
    try:
        version, room_min = int(value["version"]), int(value["room_min"])
    except (KeyError, TypeError, ValueError):
        return None
    if version < 1 or room_min < 1 or room_min > version:
        return None
    return {"version": version, "room_min": room_min}


def room_build_reason(room, joiner) -> tuple[str, str] | None:
    """(reason, action) when two builds cannot share a room, else None.

    Shaped like `nsr_content.room_content_reason` so the lobby treats both
    kinds of mismatch the same way. The text comes from `room_reason` below,
    which leads into the update rather than naming a version number.
    """
    r, j = parse_build_descriptor(room), parse_build_descriptor(joiner)
    if r is None or j is None:
        return None
    verdict = room_reason(r["version"], j["version"], r["room_min"], j["room_min"])
    if verdict is None:
        return None
    who, text = verdict
    return (text, BUILD_UPDATE_ACTION if who == "joiner" else HOST_BUILD_UPDATE_ACTION)


def room_reason(host_version: int, joiner_version: int, host_min: int,
                joiner_min: int) -> tuple[str, str] | None:
    """(who, plain text) when a room must refuse this pairing, else None.

    The text leads into the update rather than naming a mismatch, because
    "update and try again" is something a player can act on and a version
    number is not.
    """
    if joiner_version < host_min:
        return ("joiner", "The host is on a newer version. Start the launcher again to "
                          "get the update, then rejoin.")
    if host_version < joiner_min:
        return ("host", "Your version is newer than the host's. They need to start their "
                        "launcher again to get the update.")
    return None


def canonical_bytes(payload: dict) -> bytes:
    """Canonical payload bytes, for a publisher that signs detached."""
    return canonical(validate_payload(payload))
