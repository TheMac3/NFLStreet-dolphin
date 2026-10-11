#!/usr/bin/env python3
"""nsr_patcher.py: one-click builder of the NFLStreet rollback netplay game image.

For friends testing the POC. Double-click NSR-Patcher.exe (built from this file
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
  4. writes nsr-image.json (recipe, source/output digests, fingerprint; no
     paths) and shows the result's fingerprint. Netplay needs byte-identical
     images on every machine, so the lobby compares fingerprints (and this
     says whether the image matches the reference build).

The milestone-1 lockstep image (`freeze seed input netplay`) is no longer
built here: it can't play rollback, and the rollback image can't play
milestone-1 lockstep, so the netplay mode is a property of the image. An old
lockstep nsr-net.iso is recognized (LEGACY_OUTPUTS) and reported as needing a
rebuild.

Nothing is downloaded or uploaded; Reborn's own file is only read. The
patched image is each player's own copy, made from their own install.

Usage: NSR-Patcher.exe [path\\to\\NSR.iso]      (no argument: search, then ask)
       python3 tools/nsr_patcher.py [NSR.iso] [--out DIR] [--no-pause]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import isopatch  # noqa: E402

OUTPUT_NAME = "nsr-net.iso"
MANIFEST_NAME = "nsr-image.json"  # sidecar describing nsr-net.iso (no paths)
# Versioned name of the patch recipe friends build. Bump the suffix whenever
# isopatch's rollback bytes change (then update KNOWN_INPUTS' outputs).
ROLLBACK_RECIPE = "gn7e69-r0-rollback-v1"
NETPLAY_MODE = "rollback"  # what this image plays (nsr_lobby.RELEASE_MODE)
# Where the Reborn install was found, for nsr-netplay.bat to point Dolphin's
# texture loading at Reborn's own HD texture pack (read-only; nothing in the
# Reborn install is ever modified or copied).
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
            title="Find NFL Street Reborn's NSR.iso (in the Reborn install: content\\games)",
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
                 matches_reference: bool | None, reborn_install: str | None):
        self.image = image
        self.fingerprint = fingerprint
        self.reborn_label = reborn_label            # e.g. "Reborn 2026.09.12" (None: unknown version)
        self.matches_reference = matches_reference  # None when the Reborn version is unknown
        self.reborn_install = reborn_install        # for read-only textures, if found


def reborn_install_of(iso: str) -> str | None:
    """Reborn's install folder (the one holding content/textures), if it is one."""
    games_dir = os.path.dirname(os.path.abspath(iso))
    install = os.path.dirname(os.path.dirname(games_dir))
    return install if os.path.isdir(os.path.join(install, "content", "textures")) else None


def saved_reborn_install(out_dir: str) -> str | None:
    """The Reborn install recorded next to nsr-net.iso by the last build
    (REBORN_PATH_NAME), if it still exists. Lets the launcher use Reborn's HD
    textures when the image was built by NSR-Patcher.exe rather than by the
    launcher itself."""
    try:
        with open(os.path.join(out_dir, REBORN_PATH_NAME)) as f:
            path = f.read().strip()
    except OSError:
        return None
    return path if path and os.path.isdir(os.path.join(path, "content", "textures")) else None


def write_manifest(out_dir: str, digest_in: str, label: str | None, digest_out: str) -> None:
    """nsr-image.json: what nsr-net.iso is. Digests and labels only; no paths
    or Reborn content (release plan 3.1)."""
    manifest = {"recipe": ROLLBACK_RECIPE, "mode": NETPLAY_MODE,
                "patches": list(isopatch.NETPLAY_SET),
                "source_label": label, "source_sha256": digest_in,
                "output_sha256": digest_out, "fingerprint": fingerprint(digest_out),
                "ratings_pack_sha256": None,
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


def prepare_image(iso: str, out_dir: str, progress=None) -> Prepared:
    """Check Reborn's image, build nsr-net.iso in out_dir, fingerprint it.
    progress(stage, done, total) with stage in {"check", "build", "verify"}.
    Raises isopatch.PatchError if the patches don't fit this Reborn version.
    The build goes to a temporary file that replaces nsr-net.iso only when
    complete, so a failure leaves any previous image untouched and no partial
    file behind. Reborn's files are only read."""
    step = (lambda st: (lambda d, t: progress(st, d, t))) if progress else (lambda st: None)
    digest_in = sha256(iso, progress=step("check")) if progress else sha256(iso, "checking Reborn image")
    known = KNOWN_INPUTS.get(digest_in)
    out = os.path.join(out_dir, OUTPUT_NAME)
    tmp = out + ".partial"
    if os.path.abspath(iso) in (os.path.abspath(out), os.path.abspath(tmp)):
        raise isopatch.PatchError("the Reborn image and the output are the same file")
    try:
        isopatch.build(iso, tmp, set(isopatch.NETPLAY_SET),
                       progress=step("build") if progress else (lambda d, t: _bar("copying + patching", d, t)))
        if not progress:
            print()
        digest_out = sha256(tmp, progress=step("verify")) if progress else sha256(tmp, "fingerprinting result")
        os.replace(tmp, out)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise
    write_manifest(out_dir, digest_in, known[0] if known else None, digest_out)
    install = reborn_install_of(iso)
    if install:
        with open(os.path.join(out_dir, REBORN_PATH_NAME), "w") as f:
            f.write(install)
    return Prepared(out, fingerprint(digest_out), known[0] if known else None,
                    (digest_out == known[1]) if known else None, install)


class ImageStatus:
    """What an existing nsr-net.iso is: kind "release" (a known rollback
    output), "custom" (current recipe from an unrecognized Reborn, per its
    manifest), "legacy" (an older launcher's lockstep image: rebuild), or
    "unknown" (e.g. a ratings-pack build, docs/roster-editing.md). Only
    "legacy" is refused: the lobby's fingerprint check keeps any other
    mismatch out of a room, and refusing "unknown" would block ratings packs."""

    def __init__(self, fingerprint: str, kind: str, note: str = ""):
        self.fingerprint = fingerprint
        self.kind = kind
        self.note = note

    @property
    def playable(self) -> bool:
        return self.kind != "legacy"


def image_status(out_dir: str, progress=None) -> ImageStatus | None:
    """Classify an existing nsr-net.iso in out_dir (None if missing). Hashes the
    whole image (a few seconds) so a stale or damaged copy is never reported
    as ready."""
    path = os.path.join(out_dir, OUTPUT_NAME)
    if not os.path.isfile(path):
        return None
    digest = sha256(path, progress=progress or (lambda d, t: None))
    fp = fingerprint(digest)
    if digest in {out for _, out in KNOWN_INPUTS.values()}:
        return ImageStatus(fp, "release")
    if digest in LEGACY_OUTPUTS:
        return ImageStatus(fp, "legacy", f"an older {LEGACY_OUTPUTS[digest]} build")
    manifest = read_manifest(out_dir)
    if manifest.get("recipe") == ROLLBACK_RECIPE and manifest.get("output_sha256") == digest:
        return ImageStatus(fp, "custom", "unrecognized Reborn version")
    return ImageStatus(fp, "unknown", "not built by this version")


def image_fingerprint(out_dir: str, progress=None) -> str | None:
    """Fingerprint of an existing nsr-net.iso in out_dir (None if missing)."""
    path = os.path.join(out_dir, OUTPUT_NAME)
    if not os.path.isfile(path):
        return None
    return fingerprint(sha256(path, progress=progress or (lambda d, t: None)))


def run(iso: str | None, out_dir: str) -> int:
    print("NFLStreet rollback netplay image builder\n")
    if not iso:
        print("Looking for NFL Street Reborn...")
        iso = find_reborn_iso() or ask_for_iso()
    if not iso or not os.path.isfile(iso):
        print("Couldn't find Reborn's NSR.iso. Run this again with its path, e.g.\n"
              '  NSR-Patcher.exe "C:\\...\\NFL Street Reborn\\content\\games\\NSR.iso"')
        return 1
    print(f"Reborn image: {iso}\n")
    started = time.time()
    try:
        r = prepare_image(iso, out_dir)
    except isopatch.PatchError as e:
        print(f"\n\nFAILED: {e}\nThis Reborn version needs updated patches. Nothing was written.")
        return 1
    print(f"  {r.reborn_label or 'unknown Reborn version (patches applied cleanly)'}; "
          f"done in {time.time() - started:.0f}s")
    print(f"\nRecipe: {ROLLBACK_RECIPE}")
    print(f"Your netplay image fingerprint:  {r.fingerprint}")
    if r.matches_reference:
        print("Matches the reference build. Everyone with this fingerprint can play together.")
    elif r.matches_reference is False:
        print("WARNING: doesn't match the reference build. Don't use it for netplay; ask Mason.")
    else:
        print("Compare this fingerprint with everyone you play with: it must be identical.")
    if r.reborn_install:
        print(f"Reborn textures will be used read-only from: {os.path.join(r.reborn_install, 'content', 'textures')}")
    print("\nYour Reborn install was only read, never changed.")
    print(f"Next: start a game with the NSR Netplay launcher (it uses {OUTPUT_NAME} from this folder).")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("iso", nargs="?", help="path to Reborn's NSR.iso (default: find it)")
    default_out = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False)
                                                  else __file__))
    ap.add_argument("--out", default=default_out, help="folder for nsr-net.iso (default: next to this program)")
    ap.add_argument("--no-pause", action="store_true", help="don't wait for Enter at the end")
    a = ap.parse_args(argv)
    try:
        rc = run(a.iso, a.out)
    except KeyboardInterrupt:
        rc = 1
    if getattr(sys, "frozen", False) and not a.no_pause:
        input("\nPress Enter to close.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
