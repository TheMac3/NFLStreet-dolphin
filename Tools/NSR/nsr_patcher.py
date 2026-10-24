#!/usr/bin/env python3
"""nsr_patcher.py: one-click builder of the NFLStreet rollback netplay game image.

Retail sources with the exact native DOL route to ownimage_build's separately
verified recipe. Native builds currently require an output under local/ and
stock content/draft policy; see docs/ownimage.md for unresolved provenance and
runtime qualification. Conflicting file references fail closed.

For friends testing the POC. Double-click NFLStreetRollback-Patcher.exe (built from this file
with PyInstaller) inside the NFLStreet netplay Dolphin folder, and it:

  1. finds the NFL Street Reborn game image (<Reborn install>/content/games/
     NSR.iso) in the usual install locations, or asks with a file picker,
  2. checks it's the Reborn version these patches were written for,
  3. writes nsr-net.iso next to itself: Reborn's image with the rollback
     netplay patches (recipe ROLLBACK_RECIPE = tools/isopatch.py
     `freeze seed rollback`; menus run in confirmed lockstep and matches in
     rollback, see docs/release-plan.md section 3). It builds into a
     temporary file and only replaces nsr-net.iso once the new copy is
     complete, so a failed build never destroys a working image,
  4. writes nsr-image.json (recipe, source/output digests, fingerprint,
     content version if any; no paths) and shows the result's fingerprint. Netplay needs byte-identical
     images on every machine, so the lobby compares fingerprints (and this
     says whether the image matches the reference build).

The milestone-1 lockstep image (`freeze seed input netplay`) is no longer
built here: it can't play rollback, and the rollback image can't play
milestone-1 lockstep, so the netplay mode is a property of the image. An old
lockstep nsr-net.iso is recognized (LEGACY_OUTPUTS) and reported as needing a
rebuild.

Nothing is downloaded or uploaded here; Reborn's own file is only read. The
launcher can pass a verified content update (nsr_content.Content: our own
ratings-pack edits, never game bytes) to prepare_image, which then also
rewrites the roster file inside the fresh copy and requires the result to
match the fingerprint the signed manifest predicts for this Reborn input. The
patched image is each player's own copy, made from their own install.

Usage: NFLStreetRollback-Patcher.exe [path\\to\\NSR.iso]      (no argument: search, then ask)
       python3 tools/nsr_patcher.py [NSR.iso] [--out DIR] [--no-pause]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import isopatch  # noqa: E402
import draftmodes  # noqa: E402
import ownimage  # local retail metadata verification
import ownimage_build  # independently authored retail recipe
import nsr_content  # noqa: E402  (content updates: our ratings packs, docs/content-updates.md)

OUTPUT_NAME = "nsr-net.iso"
MANIFEST_NAME = "nsr-image.json"  # sidecar describing nsr-net.iso (no paths)
# Versioned name of the patch recipe friends build. Bump the suffix whenever
# isopatch's rollback bytes change (then update KNOWN_INPUTS' outputs).
ROLLBACK_RECIPE = "gn7e69-r0-rollback-v1"
NETPLAY_MODE = "rollback"  # what this image plays (nsr_lobby.RELEASE_MODE)
# Source install discovery for the first local texture/controller import.
# Normal play uses our Assets store and does not require this path to exist.
REBORN_PATH_NAME = "nsr-reborn.txt"

# Known Reborn images (SHA-256 of app/content/games/NSR.iso) and the netplay
# image each one produces. Add a row when Reborn ships an update and the
# patches still apply (isopatch's byte checks will refuse otherwise).
KNOWN_INPUTS = {
    # Reborn 3.0.0 launcher, game-patch chain label "2026.09.12".
    # Output = ROLLBACK_RECIPE (isopatch freeze seed rollback as of 67e0285,
    # 2026-10-09): byte-identical to local/work/rb3.iso, the image every
    # rollback/stage-4 test ran on. Fingerprint B57A-8270-68C2.
    "9a6fdedbdff2b2f4ebcc346ccf0bacc97e0489a15be80c648249d0b36c6270ce":
        ("Reborn 2026.09.12", "b57a827068c2e4172dda3f863db9fb446982cefddcf45708b0df6c2ebb7c6a83"),
}
# Images earlier launchers built, so an old nsr-net.iso is reported as stale
# instead of "unrecognized". Milestone-1 lockstep (freeze seed input netplay)
# from Reborn 2026.09.12, fingerprint F3FE-3498-D43D.
LEGACY_OUTPUTS = {
    "f3fe3498d43d4e346419352b3b376810dbc675d82eb5cc1d1ee517e9193601d8": "lockstep (milestone 1)",
}


def search_roots() -> list[str]:
    env = os.environ
    roots = [env.get("LOCALAPPDATA", ""), os.path.join(env.get("LOCALAPPDATA", ""), "Programs"),
             env.get("ProgramFiles", ""), env.get("ProgramFiles(x86)", ""), env.get("APPDATA", ""),
             os.path.expanduser("~"), os.path.join(os.path.expanduser("~"), "Desktop"),
             os.path.join(os.path.expanduser("~"), "Downloads"), os.path.join(os.path.expanduser("~"), "Games")]
    for drive in "CDEFG":
        roots += [f"{drive}:\\", f"{drive}:\\Games"]
    return [r for r in dict.fromkeys(roots) if r and os.path.isdir(r)]


# Where NSR.iso sits under a Reborn folder: the installed layout is
# <install dir>/content/games/NSR.iso; an innoextract'ed installer shows the
# install dir as app/.
LAYOUTS = (("content", "games", "NSR.iso"), ("app", "content", "games", "NSR.iso"))


def find_reborn_iso() -> str | None:
    """Look for a folder named like *reborn* (up to 3 levels down in the usual
    places) containing the game image in one of LAYOUTS."""
    for root in search_roots():
        for dirpath, dirnames, _ in _walk(root, depth=3):
            for d in dirnames:
                if "reborn" in d.lower():
                    for layout in LAYOUTS:
                        candidate = os.path.join(dirpath, d, *layout)
                        if os.path.isfile(candidate):
                            return candidate
    return None


def _walk(root: str, depth: int):
    try:
        entries = [e for e in os.scandir(root) if e.is_dir(follow_symlinks=False)]
    except OSError:
        return
    yield root, [e.name for e in entries], []
    if depth > 0:
        for e in entries:
            if not e.name.startswith((".", "$")) and e.name.lower() not in ("windows", "node_modules"):
                yield from _walk(e.path, depth - 1)


def ask_for_iso() -> str | None:
    try:
        import tkinter
        from tkinter import filedialog
        root = tkinter.Tk()
        root.withdraw()
        path = filedialog.askopenfilename(
            title="Choose your NFL Street 2 retail ISO or Reborn NSR.iso",
            filetypes=[("Reborn game image", "NSR.iso"), ("GameCube image", "*.iso")])
        root.destroy()
        return path or None
    except Exception:
        return None


def sha256(path: str, label: str = "", progress=None) -> str:
    """SHA-256 of a file. progress(done, total) is called as it reads; with a
    label and no callback, a console bar is drawn."""
    h = hashlib.sha256()
    total = os.path.getsize(path)
    done = 0
    report = progress or (lambda d, t: _bar(label, d, t))
    with open(path, "rb") as f:
        while chunk := f.read(16 << 20):
            h.update(chunk)
            done += len(chunk)
            report(done, total)
    if not progress:
        print()
    return h.hexdigest()


def _bar(label: str, done: int, total: int) -> None:
    pct = done * 100 // max(total, 1)
    print(f"\r  {label:<22} [{'#' * (pct // 4):<25}] {pct:3d}%", end="", flush=True)


def fingerprint(digest: str) -> str:
    """Short, readable form for players to compare (first 12 hex digits)."""
    return f"{digest[:4]}-{digest[4:8]}-{digest[8:12]}".upper()


class Prepared:
    """Result of prepare_image()."""

    def __init__(self, image: str, fingerprint: str, reborn_label: str | None,
                 matches_reference: bool | None, reborn_install: str | None,
                 content: "nsr_content.Content | None" = None, source_sha256: str = "",
                 base_content_known: bool = False):
        self.image = image
        self.fingerprint = fingerprint
        self.reborn_label = reborn_label            # e.g. "Reborn 2026.09.12" (None: unknown version)
        self.matches_reference = matches_reference  # None for unknown inputs or composed draft images
        self.reborn_install = reborn_install        # for read-only textures, if found
        self.content = content                      # the applied content update, if any
        self.source_sha256 = source_sha256
        # A known base with our authored draft code can still receive ratings
        # updates automatically; unrelated custom images keep the opt-in policy.
        self.base_content_known = base_content_known


def reborn_install_of(iso: str) -> str | None:
    """Reborn's install folder (the one holding content/textures), if it is one."""
    games_dir = os.path.dirname(os.path.abspath(iso))
    install = os.path.dirname(os.path.dirname(games_dir))
    expected_layout = (os.path.basename(games_dir).lower() == "games"
                       and os.path.basename(os.path.dirname(games_dir)).lower() == "content")
    return install if (os.path.isdir(os.path.join(install, "content", "textures"))
                       or expected_layout and os.path.isdir(install)) else None


def saved_reborn_install(out_dir: str) -> str | None:
    """The Reborn install recorded next to nsr-net.iso by the last build
    (REBORN_PATH_NAME), if it still exists. Lets the launcher use Reborn's HD
    textures when the image was built by NFLStreetRollback-Patcher.exe rather than by the
    launcher itself."""
    try:
        with open(os.path.join(out_dir, REBORN_PATH_NAME)) as f:
            path = f.read().strip()
    except OSError:
        return None
    return path if path and (os.path.isdir(os.path.join(path, "content", "textures"))
                             or os.path.isdir(os.path.join(path, "content", "games"))) else None


def write_manifest(out_dir: str, digest_in: str, label: str | None, digest_out: str,
                   content: "nsr_content.Content | None" = None,
                   draft_order: str = "alternating", draft_first: str = "stock") -> None:
    """nsr-image.json: what nsr-net.iso is. Digests and labels only; no paths
    or Reborn content (release plan 3.1). "content" records the applied
    content update (version, manifest serial, pack digests) or is null."""
    packs = [p["sha256"] for p in content.release["packs"]] if content else []
    manifest = {"recipe": ROLLBACK_RECIPE, "mode": NETPLAY_MODE,
                "patches": sorted(set(isopatch.NETPLAY_SET) | draftmodes.patch_names(draft_order, draft_first)),
                "draft_order": draft_order, "draft_first": draft_first,
                "draft_recipe": "gn7e69-r0-draft-v1",
                "source_label": label, "source_sha256": digest_in,
                "output_sha256": digest_out, "fingerprint": fingerprint(digest_out),
                "ratings_pack_sha256": packs[0] if len(packs) == 1 else None,
                "content": ({"version": content.version, "serial": content.serial, "packs": packs}
                            if content else {"version": 0, "serial": 0, "packs": []}
                            if digest_in in KNOWN_INPUTS and draftmodes.patch_names(draft_order, draft_first) else None),
                "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    tmp = os.path.join(out_dir, MANIFEST_NAME + ".tmp")
    with open(tmp, "w") as f:
        json.dump(manifest, f, indent=1)
    os.replace(tmp, os.path.join(out_dir, MANIFEST_NAME))


def read_manifest(out_dir: str) -> dict:
    try:
        with open(os.path.join(out_dir, MANIFEST_NAME)) as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def apply_content(image: str, build: dict, content: "nsr_content.Content") -> None:
    """Write a content update's ratings edits into the roster file of a fresh,
    unpublished build (the .partial file). The roster file must hash to the
    release's template_sha256 first; eadb validates every edit (player
    present, original values as expected, per-field storage range) before any
    byte changes. CRC words are preserved exactly as the editor does
    (docs/roster-editing.md), so this matches the publisher's prediction."""
    import discpatch  # noqa: PLC0415  (only content builds need the FST/DB tooling)
    import eadb  # noqa: PLC0415
    template = discpatch.read_file(image, build["roster_file"])
    if hashlib.sha256(template).hexdigest() != build["template_sha256"]:
        raise nsr_content.ContentError("the roster file in this build differs from the one the update "
                                       "was made for")
    try:
        replacement, _ = eadb.apply_edits(template, content.edits)
    except ValueError as exc:
        raise nsr_content.ContentError(f"the update's ratings don't fit this game copy: {exc}") from None
    discpatch.replace_in_place(image, build["roster_file"], replacement,
                               expect_sha256=build["template_sha256"])


def retail_fingerprint(digest: str, variant: str = "retail") -> str:
    """Agree the exact named variant and full output digest within the retail family."""
    if variant not in ("retail", *ownimage.VARIANTS):
        raise ValueError("unknown retail image variant; rebuild the image")
    return "retail:" + variant + ":" + digest.lower()


def is_retail_source(image: str) -> bool:
    """Route exact retail executables to the strict retail builder, never a fallback."""
    try:
        with open(image, "rb") as stream:
            size = stream.seek(0, 2)
            data = ownimage.header(stream, size)
            return ownimage.dol_metadata(stream, data, size)["sha1"] == ownimage.RETAIL["dol_sha1"]
    except (OSError, ValueError):
        return False


def prepare_retail_image(iso: str, out_dir: str, progress=None) -> Prepared:
    """Build a separate retail recipe; refuse conflicting references transactionally.

    Use a separate launcher directory under local/ for this experimental family.
    Retail has native rosters, controls and presentation. Signed Reborn content
    and nonstock draft policies need their own retail qualification first.
    """
    out = os.path.join(out_dir, OUTPUT_NAME)
    partial = out + ".retail-partial"
    if os.path.exists(partial) or os.path.islink(partial):
        raise isopatch.PatchError("a retail partial output already exists; inspect it before retrying")
    if os.path.exists(out) and os.path.samefile(iso, out):
        raise isopatch.PatchError("the retail source aliases the output")
    published_partial = False
    try:
        result = ownimage_build.build(iso, partial,
            progress=(lambda d, t: progress("build", d, t)) if progress else None)
        published_partial = True
        digest = result["output_sha256"]
        manifest = {**result, "base_family": ownimage_build.FAMILY, "mode": NETPLAY_MODE,
                    "fingerprint": retail_fingerprint(digest, result["variant"]), "patches": list(isopatch.ROLLBACK_SET),
                    "draft_order": "alternating", "draft_first": "stock", "content": None,
                    "source_label": "NFL Street 2 USA rev 0 (" + result["variant"] + ")",
                    "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        temporary_manifest = os.path.join(out_dir, MANIFEST_NAME + ".tmp")
        with open(temporary_manifest, "w") as stream:
            json.dump(manifest, stream, indent=1)
        os.replace(partial, out)
        os.replace(temporary_manifest, os.path.join(out_dir, MANIFEST_NAME))
    finally:
        if published_partial and os.path.isfile(partial):
            os.unlink(partial)
    return Prepared(out, retail_fingerprint(digest, result["variant"]), manifest["source_label"], None, None,
                    source_sha256=result["source_sha256"])


def prepare_image(iso: str, out_dir: str, progress=None, content: "nsr_content.Content | None" = None,
                  content_strict: bool = True, *,
                  draft_order: str = "alternating", draft_first: str = "stock") -> Prepared:
    """Check Reborn's image, build nsr-net.iso in out_dir, fingerprint it.
    Draft choices are applied after validating a signed content output, and
    the final fingerprint covers both.
    progress(stage, done, total) with stage in {"check", "build", "verify"}.
    Raises isopatch.PatchError if the patches don't fit this Reborn version.
    The build goes to a temporary file that replaces nsr-net.iso only when
    complete, so a failure leaves any previous image untouched and no partial
    file behind. Reborn's files are only read.

    content: a verified content update. Its release must list this Reborn
    input (else ContentError, or with content_strict=False a plain build
    without content), the edits are applied to the fresh copy, and the result
    must equal the release's output_sha256 exactly (ContentError otherwise,
    and the previous nsr-net.iso is kept)."""
    if not os.path.isfile(iso):
        raise FileNotFoundError("Reborn's game file (NSR.iso) is needed for this rebuild. "
                                "Locate Reborn in Settings → Advanced. Your existing game copy is kept.")
    install = reborn_install_of(iso)
    if install and Path(out_dir).resolve().is_relative_to(Path(install).resolve()):
        raise ValueError("Keep NFLStreetRollback in its own folder outside Reborn before building/importing.")
    choices = draftmodes.patch_names(draft_order, draft_first)
    if is_retail_source(iso):
        if content or choices:
            raise isopatch.PatchError("retail v1 supports native content and stock draft policy only")
        return prepare_retail_image(iso, out_dir, progress)
    step = (lambda st: (lambda d, t: progress(st, d, t))) if progress else (lambda st: None)
    digest_in = sha256(iso, progress=step("check")) if progress else sha256(iso, "checking Reborn image")
    known = KNOWN_INPUTS.get(digest_in)
    build = content.build_for(digest_in) if content else None
    if content and build is None:
        if content_strict:
            raise nsr_content.ContentError(f"content v{content.version} doesn't support this Reborn version")
        content = None
    if build and content.release["recipe"] != ROLLBACK_RECIPE:
        raise nsr_content.ContentError("this update is for a different NFLStreetRollback version")
    if build and known and known[1] != build["base_sha256"]:
        raise nsr_content.ContentError("this update was made for a different patch build")
    out = os.path.join(out_dir, OUTPUT_NAME)
    tmp = out + ".partial"
    if os.path.abspath(iso) in (os.path.abspath(out), os.path.abspath(tmp)):
        raise isopatch.PatchError("the Reborn image and the output are the same file")
    try:
        isopatch.build(iso, tmp, set(isopatch.NETPLAY_SET) | (choices if not build else set()),
                       progress=step("build") if progress else (lambda d, t: _bar("copying + patching", d, t)))
        if not progress:
            print()
        if build:
            apply_content(tmp, build, content)
        digest_out = sha256(tmp, progress=step("verify")) if progress else sha256(tmp, "fingerprinting result")
        if build and digest_out != build["output_sha256"]:
            raise nsr_content.ContentError(
                f"the updated game copy came out as {fingerprint(digest_out)}, not the expected "
                f"{fingerprint(build['output_sha256'])}; your current copy was kept")
        # The signed release authenticates the complete ratings/base image.
        # Verify that exact digest first, then layer bundled authored draft
        # code over the verified copy. The final image fingerprint agrees both
        # content and draft configuration across peers, without weakening the
        # signed digest check or changing the ratings-only pack format.
        if build and choices:
            with open(tmp, "r+b") as draft_image:
                draftmodes.apply(draft_image, set(isopatch.NETPLAY_SET) | choices)
            digest_out = sha256(tmp, progress=step("verify"))
        os.replace(tmp, out)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise
    label = known[0] if known else (build["label"] if build else None)
    write_manifest(out_dir, digest_in, label, digest_out, content if build else None, draft_order, draft_first)
    install = reborn_install_of(iso)
    if install:
        with open(os.path.join(out_dir, REBORN_PATH_NAME), "w") as f:
            f.write(install)
    # A signed ratings image was checked above before optional draft code.
    # A composed image is custom; do not claim its final digest was signed.
    matches = None if choices else (True if build else ((digest_out == known[1]) if known else None))
    return Prepared(out, fingerprint(digest_out), label, matches, install,
                    content if build else None, digest_in, bool(known or build))


class ImageStatus:
    """What an existing nsr-net.iso is: kind "release" (a known rollback
    output), "content" (built by the launcher with a content update, per its
    manifest), "custom" (current recipe from an unrecognized Reborn, per its
    manifest), "legacy" (an older launcher's lockstep image: rebuild), or
    "unknown" (e.g. a ratings-pack build, docs/roster-editing.md). Only
    "legacy" is refused: the lobby's fingerprint check keeps any other
    mismatch out of a room, and refusing "unknown" would block ratings packs.

    content: {"version", "serial"} of the installed content (version 0 for an
    unedited release image), None when unknown (custom/unknown builds)."""

    def __init__(self, fingerprint: str, kind: str, note: str = "", content: dict | None = None,
                 source_sha256: str = ""):
        self.fingerprint = fingerprint
        self.kind = kind
        self.note = note
        self.content = content
        self.source_sha256 = source_sha256

    @property
    def playable(self) -> bool:
        return self.kind not in {"legacy", "invalid-retail"}


def image_status(out_dir: str, progress=None) -> ImageStatus | None:
    """Classify an existing nsr-net.iso in out_dir (None if missing). Hashes the
    whole image (a few seconds) so a stale or damaged copy is never reported
    as ready."""
    path = os.path.join(out_dir, OUTPUT_NAME)
    if not os.path.isfile(path):
        return None
    digest = sha256(path, progress=progress or (lambda d, t: None))
    fp = fingerprint(digest)
    manifest = read_manifest(out_dir)
    source = manifest.get("source_sha256") if isinstance(manifest.get("source_sha256"), str) else ""
    if (manifest.get("recipe") == ownimage_build.RECIPE and manifest.get("base_family") == ownimage_build.FAMILY
            and manifest.get("output_sha256") == digest
            and manifest.get("variant") in ("retail", *ownimage.VARIANTS)):
        return ImageStatus(retail_fingerprint(digest, manifest["variant"]), "retail",
                           "Native rosters and controls; " + manifest["variant"] + "; experimental recipe",
                           source_sha256=source)
    if manifest.get("base_family") == ownimage_build.FAMILY or manifest.get("recipe") == ownimage_build.RECIPE:
        return ImageStatus("", "invalid-retail", "an incomplete or stale retail manifest; rebuild the image")
    ours = manifest.get("recipe") == ROLLBACK_RECIPE and manifest.get("output_sha256") == digest
    applied = nsr_content.parse_descriptor(
        {k: (manifest.get("content") or {}).get(k) for k in ("version", "serial")}
        if isinstance(manifest.get("content"), dict) else None)
    order, first = manifest.get("draft_order", "alternating"), manifest.get("draft_first", "stock")
    draft_note = (f"Pickup draft: {order}; first: {first}" if order in draftmodes.ORDERS and first in draftmodes.FIRSTS
                  else "Custom game copy; draft setup unknown")
    # Checked before the release table: a content release without packs
    # (e.g. "back to the stock ratings") builds the unedited release bytes
    # but still counts as that content version.
    if ours and applied and applied["version"] > 0:
        return ImageStatus(fp, "content", f"game content v{applied['version']}; {draft_note}", content=applied,
                           source_sha256=source)
    if digest in {out for _, out in KNOWN_INPUTS.values()}:
        source = source or next(k for k, (_, out) in KNOWN_INPUTS.items() if out == digest)
        return ImageStatus(fp, "release", content=nsr_content.content_descriptor(0, 0), source_sha256=source)
    if digest in LEGACY_OUTPUTS:
        return ImageStatus(fp, "legacy", f"an older {LEGACY_OUTPUTS[digest]} build")
    if ours:
        return ImageStatus(fp, "custom", draft_note, content=applied, source_sha256=source)
    return ImageStatus(fp, "unknown", "not built by this version")


def image_fingerprint(out_dir: str, progress=None) -> str | None:
    """Fingerprint of an existing nsr-net.iso in out_dir (None if missing)."""
    path = os.path.join(out_dir, OUTPUT_NAME)
    if not os.path.isfile(path):
        return None
    digest = sha256(path, progress=progress or (lambda d, t: None))
    manifest = read_manifest(out_dir)
    if (manifest.get("recipe") == ownimage_build.RECIPE and manifest.get("base_family") == ownimage_build.FAMILY
            and manifest.get("output_sha256") == digest
            and manifest.get("variant") in ("retail", *ownimage.VARIANTS)):
        return retail_fingerprint(digest, manifest["variant"])
    if manifest.get("base_family") == ownimage_build.FAMILY or manifest.get("recipe") == ownimage_build.RECIPE:
        raise ValueError("incomplete or stale retail manifest; rebuild the image")
    return fingerprint(digest)


def run(iso: str | None, out_dir: str, draft_order="alternating", draft_first="stock") -> int:
    """Build the chosen image recipe and show its room compatibility fingerprint."""
    print("NFLStreetRollback netplay image builder\n")
    if not iso:
        print("Looking for NFL Street Reborn...")
        iso = find_reborn_iso() or ask_for_iso()
    if not iso or not os.path.isfile(iso):
        print("Couldn't find Reborn's NSR.iso. Run this again with its path, e.g.\n"
              '  NFLStreetRollback-Patcher.exe "C:\\...\\NFL Street Reborn\\content\\games\\NSR.iso"')
        return 1
    print(f"Source image: {iso}\n")
    started = time.time()
    try:
        r = prepare_image(iso, out_dir, draft_order=draft_order, draft_first=draft_first)
    except isopatch.PatchError as e:
        print(f"\n\nFAILED: {e}\nYour source and previous image were kept.")
        return 1
    print(f"  {r.reborn_label or 'unknown Reborn version (patches applied cleanly)'}; "
          f"done in {time.time() - started:.0f}s")
    print(f"\nRecipe: {ownimage_build.RECIPE if r.fingerprint.startswith('retail:') else ROLLBACK_RECIPE}")
    print(f"Pickup draft: {draft_order}; first: {draft_first}")
    print(f"Your netplay image fingerprint:  {r.fingerprint}")
    if r.matches_reference:
        print("Matches the reference build. Everyone with this fingerprint can play together.")
    elif r.matches_reference is False:
        print("WARNING: doesn't match the reference build. Don't use it for netplay; ask Mason.")
    else:
        print("Compare this fingerprint with everyone you play with: it must be identical.")
    if r.reborn_install:
        print("The launcher will import textures locally from your Reborn install on first start.")
    print("\nYour source image was only read, never changed.")
    print(f"Next: start a game with the NFLStreetRollback launcher (it uses {OUTPUT_NAME} from this folder).")
    return 0


def main(argv=None) -> int:
    """Accept explicit image setup choices for CLI and packaged patcher users."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("iso", nargs="?", help="your retail ISO or Reborn NSR.iso (default: find Reborn)")
    default_out = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False)
                                                  else __file__))
    ap.add_argument("--out", default=default_out, help="folder for nsr-net.iso (default: next to this program)")
    ap.add_argument("--no-pause", action="store_true", help="don't wait for Enter at the end")
    ap.add_argument("--draft-order", choices=draftmodes.ORDERS, default="alternating")
    ap.add_argument("--draft-first", choices=draftmodes.FIRSTS, default="stock",
                    help="native first side, or an explicit in-game team (not host identity)")
    a = ap.parse_args(argv)
    try:
        rc = run(a.iso, a.out, a.draft_order, a.draft_first)
    except KeyboardInterrupt:
        rc = 1
    if getattr(sys, "frozen", False) and not a.no_pause:
        input("\nPress Enter to close.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
