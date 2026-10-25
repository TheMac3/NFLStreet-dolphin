"""nsr_content.py: signed content-update format for NFLStreetRollback (roster/rating updates).

What a content release is (docs/content-updates.md): OUR edit data only, never
game bytes. A release names one or more ratings packs (JSON lists of
"player X: attribute Y from <expected> to <value>"), the patch recipe it
targets, and, for every supported Reborn input image (by SHA-256), the
unedited base plus the expected image-baked output. Runtime delivery retains the
base and applies a verified edits-only bundle at native database load. The
fallback rebuilds locally (nsr_patcher.prepare_image with content=...) and checks
the expected output digest. The game image itself is never distributed.

Wire format (served read-only by services/telemetry at /v1/content/...):

  manifest  {"format": ENVELOPE_FORMAT, "kid": "<key id>", "payload": "<base64url>",
             "sig": "<base64url RSASSA-PKCS1-v1_5/SHA-256 over the payload bytes>"}
  payload   canonical JSON {"format": MANIFEST_FORMAT, "channel": "friends",
             "serial": n, "issued": "...Z", "release": {...}}
  release   {"version", "published", "changelog", "recipe", "min_launcher",
             "packs": [{"sha256", "size", "name"}],
             "builds": {"<reborn sha256>": {"label", "base_sha256", "roster_file",
                                            "template_sha256", "output_sha256"}}}
  pack      canonical JSON {"format": PACK_FORMAT, "name", "edits": [{"first_name",
             "last_name", "field", "value", "expected": [ints]}]}, fetched by its
             SHA-256 (content-addressed, so cacheable forever).

The signature covers the exact payload bytes (like JWS), so no JSON
canonicalization has to agree across machines for verification. "serial"
increases with every publish or rollback; launchers refuse a manifest whose
serial is below one they already accepted (replay of an old, validly signed
manifest). Everything is validated strictly: exact key sets, types, sizes and
per-attribute value ranges; anything unexpected fails closed and the player
keeps their current image.

Room descriptors (content_descriptor / room_content_reason) are pure helpers
shared by the launcher's lobby, the direct-IP lobby and the rendezvous server.

Standard library only (the rendezvous and the PyInstaller launcher import it).
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import tempfile
import urllib.error
import urllib.request

import rsasig

ENVELOPE_FORMAT = "nsrb.content.signed.v1"
MANIFEST_FORMAT = "nsrb.content.v1"
PACK_FORMAT = "nsrb.ratings-pack.v1"
CHANNEL = "friends"
# Content format this launcher understands. A release with a larger
# min_launcher needs a newer NFLStreetRollback download (it may use fields or
# build steps this code doesn't know); bump when the release format changes.
LAUNCHER_CONTENT_FORMAT = 4  # v4: runtime passing/acceleration splits, neutral by default

# Public keys trusted to sign manifests: key id -> (modulus hex, exponent).
# Empty until Mason generates the signing key on the server
# (`services/content/content_store.py keygen`, docs/content-updates.md) and
# pastes the printed line here; with no key, launchers report content updates
# as "not set up in this download" and never apply anything. Keep the old key
# listed for one release when rotating.
# Content-signing public keys (docs/content-updates.md "Signing key handling").
# The private half lives only on the server (~/.config/nsr/content-signing-key.pem).
TRUSTED_KEYS: dict[str, tuple[str, int]] = {
    "97c6fcf83c653ae9": ("d679da7952506fca9302eb2458f7b0e8b8c3532ba2a770134813380765fa44c3c0111f3781e3ecdc63e80a85fe077633a07068a1ae437153114c8add9d49a133878767d9c4501f322db9b738500aac08e8cb6bb4620352f7cbff7d4cdf0d99ee07aa0bd96c611c9280850a08fa3cc6093d5894b11def27dd240a0ce4aad84ceb3d0b4e191a3747e0ac22d8c456c0b7286289d2a2ad66f43e900278366db88df6a2fc3f2068d652e61e9bdf6e2a39b2c5188dc088c7aedb6c3863bcdd630dcab5bf1748bafb0c9c76aafab89baf863d8e9a5e148c365af942a0360cc7b7e5ea3a46684994adabeb1a7b9b7b0398b23f424c14014eb827062d7cc07e052b4ac58528929d969c8f0424132bfdc630a0a9e10e5a53b26fb64ff40e6378ac8e0ff1219ecc4661ed325e70c4d8f80bc6f43ac3b13dda1d7115e7b910658ea307e5d00f5de15a8612ae816b83914f74957006563aa8195ecbde097f2a7fe8bca35e3036c9c6202614a007d2843d07c411d9d729339ab783bd3f479f950ba179a91ef1bb", 65537),
}
MIN_KEY_BITS = 3072

# Size limits (bytes / counts). Generous for a friends' roster, small enough
# that a hostile or broken server can't make a launcher hold much.
MAX_MANIFEST_BYTES = 64 * 1024
MAX_PACK_BYTES = 512 * 1024
MAX_PACKS = 8
MAX_EDITS = 5000
MAX_BUILDS = 16
MAX_CHANGELOG = 2000
MAX_NAME = 64
MAX_PLAYER_NAME = 32
MAX_VERSION = 1_000_000
MAX_SERIAL = 2**31 - 1

# Editable attributes (tools/eadb.py ATTRIBUTES) and the values a release may
# set. Ratings are 0..127 (the editor's 7-bit storage range for the rating
# fields); height is inches; weight is stored as pounds minus 160. These are
# deliberately a little wider than sensible gameplay values: the exact
# per-field storage range is enforced again by eadb.apply_edits against the
# real template at build time, and the publisher refuses anything outside
# this table, so a valid release never fails here.
_RATING = (0, 127)
ATTRIBUTE_LIMITS = {
    "POVR": _RATING, "PSPD": _RATING, "PCEL": (0, 99), "PAGI": _RATING, "PJUM": _RATING,
    "PCTH": _RATING, "PPSS": _RATING, "PCOV": _RATING, "PTAK": _RATING, "PBLK": _RATING,
    "PBTK": _RATING, "PHGT": (48, 96), "PWGT": (0, 255),
    "PPVL": (0, 99),
}
EXPECTED_LIMIT = (0, 65535)  # original values only need to be plausible storage values

_HEX64 = re.compile(r"[0-9a-f]{64}")
_KID = re.compile(r"[0-9a-f]{16}")
_B64URL = re.compile(r"[A-Za-z0-9_-]+")
_RECIPE = re.compile(r"[a-z0-9][a-z0-9.-]{0,63}")
_ROSTER_FILE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}(/[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}){0,3}")
_STAMP = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ")


class ContentError(ValueError):
    """A manifest, pack or content build that must not be used (fail closed)."""


# --- strict JSON ------------------------------------------------------------------
def canonical(obj) -> bytes:
    """Deterministic bytes for hashing/signing: sorted keys, no spaces, ASCII."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode("ascii")


def _reject_duplicates(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise ContentError(f"duplicate JSON key: {k}")
        out[k] = v
    return out


def _bad_constant(name):
    raise ContentError(f"JSON constant {name} is not allowed")


def strict_loads(raw: bytes):
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_reject_duplicates,
                          parse_constant=_bad_constant)
    except (UnicodeDecodeError, ValueError) as exc:
        if isinstance(exc, ContentError):
            raise
        raise ContentError(f"malformed JSON: {exc}") from None


def b64decode(value) -> bytes:
    if not isinstance(value, str) or not value or len(value) > 2 * MAX_MANIFEST_BYTES \
            or not _B64URL.fullmatch(value):
        raise ContentError("invalid base64url value")
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def b64encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _keys(obj, name: str, required: set[str]) -> dict:
    if not isinstance(obj, dict):
        raise ContentError(f"{name} must be an object")
    if set(obj) != required:
        missing, extra = sorted(required - set(obj)), sorted(set(obj) - required)
        raise ContentError(f"{name} keys differ (missing {missing}, unexpected {extra})")
    return obj


def _int(value, name: str, lo: int, hi: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not lo <= value <= hi:
        raise ContentError(f"{name} must be an integer {lo}..{hi}")
    return value


def _text(value, name: str, limit: int, empty: bool = False) -> str:
    if not isinstance(value, str) or len(value) > limit or (not empty and not value.strip()):
        raise ContentError(f"{name} must be {'a' if empty else 'a non-empty'} string of at most {limit} characters")
    if any(ord(c) < 32 and c not in "\n\t" for c in value):
        raise ContentError(f"{name} contains control characters")
    return value


def _hex64(value, name: str) -> str:
    if not isinstance(value, str) or not _HEX64.fullmatch(value):
        raise ContentError(f"{name} must be a lowercase SHA-256 hex digest")
    return value


def _stamp(value, name: str) -> str:
    if not isinstance(value, str) or not _STAMP.fullmatch(value):
        raise ContentError(f"{name} must be a UTC timestamp like 2026-10-18T12:00:00Z")
    return value


# --- ratings packs -----------------------------------------------------------------
def validate_pack(obj) -> dict:
    """Strictly check one pack object; return it unchanged."""
    _keys(obj, "pack", {"format", "name", "edits"})
    if obj["format"] != PACK_FORMAT:
        raise ContentError("unknown pack format")
    _text(obj["name"], "pack name", MAX_NAME)
    edits = obj["edits"]
    if not isinstance(edits, list) or not 1 <= len(edits) <= MAX_EDITS:
        raise ContentError(f"pack must have 1..{MAX_EDITS} edits")
    seen = set()
    for i, e in enumerate(edits, 1):
        where = f"edit {i}"
        _keys(e, where, {"first_name", "last_name", "field", "value", "expected"})
        first = _text(e["first_name"], f"{where} first_name", MAX_PLAYER_NAME, empty=True)
        last = _text(e["last_name"], f"{where} last_name", MAX_PLAYER_NAME, empty=True)
        if not (first.strip() or last.strip()) or first != first.strip() or last != last.strip():
            raise ContentError(f"{where}: player name must be trimmed and not empty")
        field = e["field"]
        if field not in ATTRIBUTE_LIMITS:
            raise ContentError(f"{where}: attribute {field!r} is not editable")
        lo, hi = ATTRIBUTE_LIMITS[field]
        _int(e["value"], f"{where} {field} value", lo, hi)
        expected = e["expected"]
        if (not isinstance(expected, list) or not 1 <= len(expected) <= 8
                or expected != sorted(set(expected))):
            raise ContentError(f"{where}: expected must be 1..8 sorted distinct original values")
        for v in expected:
            _int(v, f"{where} expected value", *EXPECTED_LIMIT)
        if expected == [e["value"]] and not (field == "PCEL" and e["value"] > 0):
            raise ContentError(f"{where}: no-op edit")
        key = (first.casefold(), last.casefold(), field)
        if key in seen:
            raise ContentError(f"{where}: duplicate player/attribute edit")
        seen.add(key)
    return obj


def make_pack(name: str, rows: list[dict]) -> dict:
    """A pack from editor rows (eadb pack rows: expected as "70|71" or a list).
    Provenance hashes are dropped: the release's builds map pins them per
    Reborn input instead. Edits are sorted so the pack hash is stable."""
    edits = []
    for r in rows:
        expected = r.get("expected", "")
        if isinstance(expected, str):
            expected = [int(v) for v in expected.split("|") if v != ""]
        edits.append({"first_name": str(r["first_name"]).strip(), "last_name": str(r["last_name"]).strip(),
                      "field": r["field"], "value": int(r["value"]),
                      "expected": sorted(set(int(v) for v in expected))})
    edits.sort(key=lambda e: (e["last_name"].casefold(), e["first_name"].casefold(), e["field"]))
    return validate_pack({"format": PACK_FORMAT, "name": name, "edits": edits})


def pack_bytes(pack: dict) -> bytes:
    raw = canonical(validate_pack(pack))
    if len(raw) > MAX_PACK_BYTES:
        raise ContentError(f"pack exceeds {MAX_PACK_BYTES} bytes")
    return raw


def parse_pack(raw: bytes, sha256: str, size: int) -> dict:
    """Bytes fetched for a manifest entry: size, hash, schema, canonical form."""
    if len(raw) != size or len(raw) > MAX_PACK_BYTES:
        raise ContentError("pack size differs from the manifest")
    if hashlib.sha256(raw).hexdigest() != sha256:
        raise ContentError("pack SHA-256 differs from the manifest")
    pack = validate_pack(strict_loads(raw))
    if canonical(pack) != raw:
        raise ContentError("pack is not in canonical form")
    return pack


def eadb_rows(packs: list[dict]) -> list[dict]:
    """All packs' edits as tools/eadb.py apply_edits rows (expected "a|b")."""
    rows, seen = [], set()
    for pack in packs:
        for e in pack["edits"]:
            key = (e["first_name"].casefold(), e["last_name"].casefold(), e["field"])
            if key in seen:
                raise ContentError(f"packs both edit {e['first_name']} {e['last_name']} {e['field']}")
            seen.add(key)
            rows.append({"first_name": e["first_name"], "last_name": e["last_name"], "field": e["field"],
                         "value": e["value"], "expected": "|".join(map(str, e["expected"]))})
    return rows


# --- releases and manifests ---------------------------------------------------------
def validate_release(obj) -> dict:
    _keys(obj, "release", {"version", "published", "changelog", "recipe", "min_launcher", "packs", "builds"})
    _int(obj["version"], "release version", 1, MAX_VERSION)
    _stamp(obj["published"], "release published")
    _text(obj["changelog"], "changelog", MAX_CHANGELOG)
    if not isinstance(obj["recipe"], str) or not _RECIPE.fullmatch(obj["recipe"]):
        raise ContentError("release recipe is malformed")
    _int(obj["min_launcher"], "min_launcher", 1, 1000)
    packs = obj["packs"]
    if not isinstance(packs, list) or len(packs) > MAX_PACKS:
        raise ContentError(f"release must list at most {MAX_PACKS} packs")
    shas = set()
    for i, p in enumerate(packs, 1):
        _keys(p, f"pack entry {i}", {"sha256", "size", "name"})
        shas.add(_hex64(p["sha256"], f"pack entry {i} sha256"))
        _int(p["size"], f"pack entry {i} size", 1, MAX_PACK_BYTES)
        _text(p["name"], f"pack entry {i} name", MAX_NAME)
    if len(shas) != len(packs):
        raise ContentError("release lists a pack twice")
    builds = obj["builds"]
    if not isinstance(builds, dict) or not 1 <= len(builds) <= MAX_BUILDS:
        raise ContentError(f"release must have 1..{MAX_BUILDS} builds")
    for source, b in builds.items():
        _hex64(source, "build source")
        _keys(b, "build", {"label", "base_sha256", "roster_file", "template_sha256", "output_sha256"})
        _text(b["label"], "build label", MAX_NAME)
        _hex64(b["base_sha256"], "build base_sha256")
        _hex64(b["template_sha256"], "build template_sha256")
        _hex64(b["output_sha256"], "build output_sha256")
        if not isinstance(b["roster_file"], str) or not _ROSTER_FILE.fullmatch(b["roster_file"]) \
                or ".." in b["roster_file"]:
            raise ContentError("build roster_file is malformed")
        if not packs and b["output_sha256"] != b["base_sha256"]:
            raise ContentError("a release without packs must build the unedited image")
    return obj


def validate_payload(obj) -> dict:
    _keys(obj, "manifest", {"format", "channel", "serial", "issued", "release"})
    if obj["format"] != MANIFEST_FORMAT:
        raise ContentError("unknown manifest format")
    if obj["channel"] != CHANNEL:
        raise ContentError("manifest is for another channel")
    _int(obj["serial"], "manifest serial", 1, MAX_SERIAL)
    _stamp(obj["issued"], "manifest issued")
    validate_release(obj["release"])
    return obj


def key_id(n: int, e: int) -> str:
    """Short, stable id of a public key: SHA-256 of "n:e" in hex, 16 digits."""
    return hashlib.sha256(f"{n:x}:{e:x}".encode()).hexdigest()[:16]


def trusted_keys(keys=None) -> dict[str, tuple[int, int]]:
    """kid -> (n, e) from TRUSTED_KEYS (or a given dict), dropping weak/odd entries."""
    out = {}
    for kid, (n_hex, e) in (TRUSTED_KEYS if keys is None else keys).items():
        n = int(n_hex, 16)
        if n.bit_length() >= MIN_KEY_BITS and e >= 3 and e % 2 == 1 and key_id(n, e) == kid:
            out[kid] = (n, e)
    return out


def sign_manifest(payload: dict, kid: str, sign) -> bytes:
    """Envelope bytes for a payload; sign(message) -> signature bytes (server side)."""
    body = canonical(validate_payload(payload))
    envelope = {"format": ENVELOPE_FORMAT, "kid": kid, "payload": b64encode(body),
                "sig": b64encode(sign(body))}
    raw = canonical(envelope)
    if len(raw) > MAX_MANIFEST_BYTES:
        raise ContentError("manifest is too large")
    return raw


def verify_manifest(raw: bytes, keys=None) -> dict:
    """Check the signature with a trusted key, then the payload schema. Returns
    the payload. Raises ContentError on anything unexpected."""
    if not isinstance(raw, (bytes, bytearray)) or len(raw) > MAX_MANIFEST_BYTES:
        raise ContentError("manifest is missing or too large")
    env = _keys(strict_loads(bytes(raw)), "manifest envelope", {"format", "kid", "payload", "sig"})
    if env["format"] != ENVELOPE_FORMAT:
        raise ContentError("unknown manifest envelope format")
    if not isinstance(env["kid"], str) or not _KID.fullmatch(env["kid"]):
        raise ContentError("manifest key id is malformed")
    trusted = trusted_keys(keys)
    if not trusted:
        raise ContentError("this download has no content-signing key")
    if env["kid"] not in trusted:
        raise ContentError("manifest is signed by an unknown key")
    body, sig = b64decode(env["payload"]), b64decode(env["sig"])
    n, e = trusted[env["kid"]]
    if not rsasig.rsa_pkcs1v15_sha256_verify(n, e, body, sig):
        raise ContentError("manifest signature is invalid")
    payload = validate_payload(strict_loads(body))
    if canonical(payload) != body:
        raise ContentError("manifest payload is not in canonical form")
    return payload


def describe(release: dict) -> str:
    """'content v5 — <first changelog line>' for the launcher."""
    first = release["changelog"].strip().splitlines()[0][:120] if release["changelog"].strip() else ""
    return f"content v{release['version']}" + (f" — {first}" if first else "")


# --- launcher-side cache and download ------------------------------------------------
CACHE_DIR = "nsr-content"  # next to nsr-net.iso: verified manifest + packs (our data only)


def _atomic_write(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".tmp-", dir=os.path.dirname(path))
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def http_get(url: str, limit: int, timeout: float = 6.0) -> bytes:
    """GET with a byte cap. Raises OSError (offline/HTTP error) or ContentError (too big)."""
    req = urllib.request.Request(url, headers={"User-Agent": "NFLStreetRollback-content/1",
                                               "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:  # noqa: S310 (https checked by caller)
            raw = r.read(limit + 1)
    except urllib.error.HTTPError as exc:
        raise OSError(f"HTTP {exc.code}") from None
    except (urllib.error.URLError, TimeoutError, ValueError) as exc:
        raise OSError(str(getattr(exc, "reason", exc))) from None
    if len(raw) > limit:
        raise ContentError("download is larger than allowed")
    return raw


def check_base_url(url: str) -> str:
    """Content base URL: https, or http only to loopback (development)."""
    url = (url or "").strip().rstrip("/")
    if url.startswith("https://") or url.startswith(("http://127.0.0.1", "http://localhost")):
        return url
    return ""


class Content:
    """A verified release plus its parsed packs, ready for nsr_patcher.prepare_image."""

    def __init__(self, payload: dict, packs: list[dict]):
        self.payload = payload
        self.release = payload["release"]
        self.serial = payload["serial"]
        self.version = self.release["version"]
        self.packs = packs
        self.edits = eadb_rows(packs)

    def build_for(self, source_sha256: str) -> dict | None:
        return self.release["builds"].get(source_sha256)

    def descriptor(self) -> dict:
        return content_descriptor(self.version, self.serial)

    def runtime_bytes(self) -> bytes:
        """A deterministic edits-only boot bundle; serial/signatures stay outside
        its identity so re-signing the same release does not change gameplay."""
        if len(self.edits) > MAX_EDITS:
            raise ContentError("Runtime releases support at most 5000 total edits; use image baking.")
        for edit in self.edits:
            for key in ("first_name", "last_name"):
                if not all(32 <= ord(c) < 127 for c in edit[key]):
                    raise ContentError("Runtime ratings currently require ASCII player names; use image baking.")
        import ratingsplit
        # Activation is signed policy, not merely the presence of a field.
        # Existing native PCEL releases must not acquire new physics on upgrade.
        split = self.release.get("recipe") == ratingsplit.RUNTIME_RECIPE
        if not split and any(edit["field"] == "PPVL" for edit in self.edits):
            raise ContentError("Republish this legacy velocity pack with the current editor for "
                               "game-start updates, or use image builds.")
        raw = canonical({"format": "nsrb.runtime-content.v4" if split else "nsrb.runtime-content.v1", "version": self.version,
                         "packs": self.packs})
        if len(raw) > 4 * 1024 * 1024:
            raise ContentError("Runtime content bundle is too large; use image baking.")
        return raw

    def validate_runtime_image(self, image: str, source_sha: str) -> None:
        """Validate the signed base/template contract and all edits without writing
        an image. The caller checks the actual image fingerprint separately."""
        import discpatch
        import eadb
        build = self.build_for(source_sha)
        if build is None:
            raise ContentError("Runtime content does not support this Reborn image.")
        template = discpatch.read_file(image, build["roster_file"])
        if hashlib.sha256(template).hexdigest() != build["template_sha256"]:
            raise ContentError("Runtime content needs the unedited rollback roster; rebuild the base once.")
        try:
            eadb.apply_edits(template, self.edits)
        except ValueError as exc:
            raise ContentError(f"Runtime edits do not fit this roster: {exc}") from None
        self.runtime_bytes()


class Cache:
    """Verified manifest and packs under <folder>/nsr-content (no game data)."""

    def __init__(self, folder: str, keys=None):
        self.root = os.path.join(folder, CACHE_DIR)
        self.keys = keys

    def _pack_path(self, sha: str) -> str:
        return os.path.join(self.root, "packs", _hex64(sha, "pack sha256") + ".json")

    def save_manifest(self, raw: bytes) -> None:
        _atomic_write(os.path.join(self.root, "manifest.json"), raw)

    def manifest(self) -> dict | None:
        """The last verified manifest, re-verified (None if absent or invalid)."""
        try:
            with open(os.path.join(self.root, "manifest.json"), "rb") as f:
                return verify_manifest(f.read(MAX_MANIFEST_BYTES + 1), self.keys)
        except (OSError, ContentError):
            return None

    def cached_pack(self, entry: dict) -> dict | None:
        try:
            with open(self._pack_path(entry["sha256"]), "rb") as f:
                return parse_pack(f.read(MAX_PACK_BYTES + 1), entry["sha256"], entry["size"])
        except (OSError, ContentError):
            return None

    def load(self, payload: dict, fetch=None, base_url: str = "") -> Content:
        """Packs for a verified payload: from the cache, else downloaded
        (fetch(url, limit) -> bytes), verified and cached. Raises OSError when
        a pack can't be downloaded, ContentError when it doesn't verify."""
        packs = []
        for entry in payload["release"]["packs"]:
            pack = self.cached_pack(entry)
            if pack is None:
                if fetch is None or not base_url:
                    raise OSError("content pack not downloaded yet")
                raw = fetch(f"{base_url}/packs/{entry['sha256']}.json", entry["size"])
                pack = parse_pack(raw, entry["sha256"], entry["size"])
                _atomic_write(self._pack_path(entry["sha256"]), raw)
            packs.append(pack)
        return Content(payload, packs)

    def activate_runtime(self, content: Content) -> None:
        """Retain the verified signed envelope as the installed release, separate
        from the newest manifest (a later failed download must keep this one)."""
        with open(os.path.join(self.root, "manifest.json"), "rb") as f:
            raw = f.read(MAX_MANIFEST_BYTES + 1)
        if verify_manifest(raw, self.keys) != content.payload:
            raise ContentError("The verified manifest changed during installation; retry the update.")
        _atomic_write(os.path.join(self.root, "active-runtime.json"), raw)

    def runtime_content(self) -> Content | None:
        """Re-verify the installed envelope and cached packs, including offline.
        Corruption raises: launching stock after silently losing edits is unsafe."""
        try:
            with open(os.path.join(self.root, "active-runtime.json"), "rb") as f:
                raw = f.read(MAX_MANIFEST_BYTES + 1)
        except FileNotFoundError:
            return None
        return self.load(verify_manifest(raw, self.keys))

    def runtime_env(self, content: Content | None, draft_order="alternating", draft_first="stock", playbooks=None) -> dict[str, str]:
        """Write an immutable ratings/settings bundle for Dolphin.
        Dolphin re-hashes these bytes once before accepting the native load."""
        import draftmodes
        raw = draftmodes.runtime_bytes(content, draft_order, draft_first)
        if playbooks:
            bundle = strict_loads(raw) if raw else {"version": 0, "packs": [],
                "draft": {"order": draft_order, "first": draft_first, "policy": 1}}
            bundle["format"] = "nsrb.runtime-content.v4" if bundle.get("format") == "nsrb.runtime-content.v4" else "nsrb.runtime-content.v3"
            bundle.setdefault("draft", {"order": draft_order, "first": draft_first, "policy": 1})
            bundle["playbooks"] = {"policy": 1, "captain": "lowest-pad", "users": playbooks}
            raw = canonical(bundle)
        if raw is None:
            return {}
        if len(raw) > 4 * 1024 * 1024:
            raise ContentError("Combined runtime settings exceed the bundle size limit.")
        sha = hashlib.sha256(raw).hexdigest()
        path = os.path.abspath(os.path.join(self.root, "runtime", sha + ".json"))
        _atomic_write(path, raw)
        return {"NSR_CONTENT_PACK": path, "NSR_CONTENT_SHA256": sha,
                "NSR_CONTENT_VERSION": str(content.version if content else 0)}

    def clear_runtime(self) -> None:
        """Retire our active pointer only after a fallback image build succeeds."""
        try:
            os.unlink(os.path.join(self.root, "active-runtime.json"))
        except FileNotFoundError:
            pass


# --- room descriptors ------------------------------------------------------------------
def content_descriptor(version: int, serial: int) -> dict:
    """What a launcher sends with create/join/hello: the installed content
    version (0 = no content update applied) and the manifest serial it came
    from (which side is newer after a rollback re-points "current")."""
    return {"version": int(version), "serial": int(serial)}


def parse_descriptor(value) -> dict | None:
    """A peer's descriptor, or None (absent: an older launcher; malformed: ignored,
    the image fingerprint check still keeps mismatched images apart)."""
    if not isinstance(value, dict) or set(value) != {"version", "serial"}:
        return None
    try:
        return content_descriptor(_int(value["version"], "version", 0, MAX_VERSION),
                                  _int(value["serial"], "serial", 0, MAX_SERIAL))
    except ContentError:
        return None


UPDATE_ACTION = "update_content"            # the joiner should Check for updates
HOST_UPDATE_ACTION = "host_update_content"  # the host is the outdated one


def room_content_reason(room, joiner) -> tuple[str, str] | None:
    """(reason, action) when a joiner's content differs from the room's, else
    None. Both sides must carry a descriptor; otherwise only the fingerprint
    decides (compatibility with launchers/room servers from before content
    updates). The side whose manifest serial is lower has not seen the newest
    update, so it is the one told what to do."""
    r, j = parse_descriptor(room), parse_descriptor(joiner)
    if r is None or j is None or r["version"] == j["version"]:
        return None
    rv, jv = r["version"], j["version"]
    if j["serial"] < r["serial"]:
        return (f"This room uses game content v{rv}, and you have v{jv}. Click “Check for updates” "
                "(Settings), then join again.", UPDATE_ACTION)
    if j["serial"] > r["serial"]:
        return (f"The host has older game content (v{rv}) than you (v{jv}). Ask the host to click "
                "“Check for updates” and create a new room.", HOST_UPDATE_ACTION)
    return (f"Game content differs (room v{rv}, yours v{jv}). Both of you: click “Check for updates”.",
            UPDATE_ACTION)
