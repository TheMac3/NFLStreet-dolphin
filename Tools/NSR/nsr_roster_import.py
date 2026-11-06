# Copyright (C) 2026 Mason Blanchard
# SPDX-License-Identifier: GPL-2.0-or-later
"""Import the roster from the player's own NFL Street Reborn install.

Our ratings editor is still catching up, so a player who wants Reborn's current
roster has nowhere to get it from today. This reads the roster out of **their
own** Reborn installation, on their own PC, and writes the attributes that
differ from our image as an ordinary ratings pack - the same edits-only CSV the
editor already produces and builds from.

The boundary is the same one `docs/local-import.md` draws for textures, and it
is not a detail: the Reborn team's roster work is not published under any
licence. A per-player local import is not redistribution; anything that leaves
the player's PC is. So this reads Reborn read-only, writes only under `local/`,
and emits **changed attributes plus provenance, never an exported roster**.
Nothing it produces may enter git, an installer, a zip, an upload, or the signed
content channel.

### Why it bakes an image instead of using "Apply ratings at game start"

Runtime injection would be the cheaper application route - no 1.4 GB rebuild -
but the launcher only ever builds its boot bundle from the *signed* content
release (`nsr_content.Content.runtime_bytes`, `docs/runtime-content.md`), and
that is the one place Reborn-derived data must never reach. Giving the runtime
path a second, unsigned source is a trust-boundary decision, not a convenience,
so this tool stops at the pack and the existing local bake applies it
(`services/ratings/core.Ratings.build`). `--build` does that in one step.

Deferred: a local unsigned runtime pack. Revisit if the rebuild cost becomes the
thing people complain about, and only together with how the fork should decide
to trust a bundle nobody signed.

### Everyone in a room needs the same import

A baked roster changes the image digest, so the fingerprint the lobby already
compares differs and the room is refused. `--build` writes the receipt beside
the image it baked, as `nsr_patcher.ROSTER_IMPORT_NAME`, and the launcher turns
that into the room descriptor `nsr_lobby.roster_descriptor` compares - so the
refusal now reads "your imported rosters differ: yours is from ..." instead of
naming Reborn versions and patches. It is the same admission ladder, one step
earlier than the fingerprint; absent on either side means "do not compare", so
the players who imported nothing are unaffected.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import discpatch
import eadb
import nsr_patcher

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "services" / "ratings"))

ROSTER_BASENAME = "template.dat"
PACKS = REPO / "local" / "ratings-packs"
RECEIPT_SUFFIX = ".import.json"

# PPVL is ours, not EA's: a virtual field over PLAY bit 790 that only our
# builds populate (docs/ratingsplit.md), so Reborn's image has nothing to say
# about it. PCEL is a real field, but a nonzero value selects the v4 runtime
# acceleration policy, which exists only on the signed path this import cannot
# use. Importing either would mean importing a decision Reborn never made.
RUNTIME_ONLY = set(eadb.INHERIT_FIELDS)
IMPORTABLE = tuple(f for f in eadb.ATTRIBUTES if f not in RUNTIME_ONLY)


class RosterImportError(ValueError):
    """Refusals a player should be able to act on, raised with what was found."""


def roster_path(image: str | Path, override: str | None = None) -> str:
    """Resolve the roster file's exact FST path, which differs between builds.

    `discpatch.read_file` takes exact paths on purpose - a basename must not
    silently select a nested file - but the caller should not have to know
    whether this image carries it at the root or under a directory. Match on
    the basename here and refuse an ambiguous image rather than guessing.
    """
    entries = discpatch.files(image)
    if override:
        if override not in entries:
            raise RosterImportError(f"{Path(image).name} has no file at {override}")
        return override
    matches = sorted(n for n in entries if Path(n).name.lower() == ROSTER_BASENAME)
    if not matches:
        raise RosterImportError(f"{Path(image).name} contains no {ROSTER_BASENAME}; "
                           "is it an NFL Street 2 image?")
    if len(matches) > 1:
        raise RosterImportError(f"{Path(image).name} contains {len(matches)} files named "
                           f"{ROSTER_BASENAME}; pass --roster-file to choose one")
    return matches[0]


def label_for(digest: str) -> str | None:
    """Name an image from the allowlist, whichever side of a build it is on.

    `KNOWN_INPUTS` maps a Reborn source digest to `(label, output digest)`, so
    the same table names our own built image as well - and without this the
    target reads as an "unrecognized build", which is a confusing thing to say
    about the copy the player is running.
    """
    for source, (text, output) in nsr_patcher.KNOWN_INPUTS.items():
        if digest in (source, output):
            return text
    return nsr_patcher.LEGACY_OUTPUTS.get(digest)


def identity(image: str | Path, override: str | None = None) -> dict:
    """Describe an image without changing it: revision, digests, Reborn label.

    The revision check is the same one the ratings editor makes, and for the
    same reason: every attribute offset in `eadb` was qualified against GN7E69
    revision 0, so another revision would read plausible nonsense.

    A Reborn newer than `nsr_patcher.KNOWN_INPUTS` is the expected case here -
    wanting its roster is the whole point - so an unrecognized digest is
    recorded rather than refused, and only the label is left unknown.
    """
    path = Path(image)
    if not path.is_file():
        raise RosterImportError(f"no such image: {path}")
    with path.open("rb") as stream:
        header = stream.read(8)
    if header[:6] != b"GN7E69":
        found = header[:6].decode("latin-1", "replace")
        raise RosterImportError(f"{path.name} is game id {found!r}, not NFL Street 2 (GN7E69)")
    if header[7] != 0:
        raise RosterImportError(f"{path.name} is GN7E69 revision {header[7]}; "
                           "the roster offsets are qualified for revision 0 only")
    digest = discpatch.sha256(path)
    roster_file = roster_path(path, override)
    roster = discpatch.read_file(path, roster_file)
    return {"path": str(path), "sha256": digest, "label": label_for(digest),
            "roster_file": roster_file, "roster_sha256": hashlib.sha256(roster).hexdigest(),
            "roster": roster}


def find_reborn(explicit: str | None) -> str:
    """Take the player's path, or the install search the patcher already does.

    Accepts an install folder as well as the image itself, because that is what
    a player has a name for. Nothing is written to either.
    """
    if explicit:
        candidate = Path(explicit)
        if candidate.is_dir():
            for layout in nsr_patcher.LAYOUTS:
                inner = candidate.joinpath(*layout)
                if inner.is_file():
                    return str(inner)
            raise RosterImportError(f"{candidate} is not a Reborn install "
                               "(no content/games/NSR.iso inside it)")
        if candidate.is_file():
            return str(candidate)
        raise RosterImportError(f"no such path: {candidate}")
    found = nsr_patcher.find_reborn_iso()
    if not found:
        raise RosterImportError("couldn't find a Reborn install; pass --reborn with the path to "
                           "your NFL Street Reborn folder or its content/games/NSR.iso")
    return found


def _values(buf: bytes) -> dict:
    """Group one roster by player name, as the editor's own catalog does.

    Reuses `eadb.editable` rather than walking PLAY again: it already collapses
    a player's team copies, applies the same field allowlist, and carries the
    per-field editable range a pack row has to satisfy.
    """
    catalog = eadb.editable(buf)
    ranges = {f["name"]: (f["min"], f["max"]) for f in catalog["fields"]}
    players = {eadb.name_key(p["first_name"], p["last_name"]): p for p in catalog["players"]}
    return {"players": players, "ranges": ranges}


def differences(ours: bytes, theirs: bytes) -> tuple[list[dict], dict]:
    """Attributes that moved, plus an account of everything left out.

    One pack row sets one attribute for every team copy of a player, so the
    cases this cannot express are skipped and counted rather than approximated:

    * a player Reborn added - a pack edits players, it cannot create them;
    * a player whose Reborn copies disagree with each other, where there is no
      single value to ask for;
    * a value outside the range the editor and the build both accept, which
      would be refused later anyway (`eadb.editable_range`).

    The counts are the point. A silent partial import would leave someone
    wondering why a player they looked up did not change.
    """
    mine, yours = _values(ours), _values(theirs)
    rows, skipped = [], {"added_players": 0, "inconsistent_copies": 0, "out_of_range": 0,
                         "missing_players": 0}
    for key, player in sorted(yours["players"].items()):
        here = mine["players"].get(key)
        if here is None:
            skipped["added_players"] += 1
            continue
        for field in IMPORTABLE:
            want, have = player["values"].get(field), here["values"].get(field)
            if want is None or have is None or want == have:
                continue
            if len(want) != 1:
                skipped["inconsistent_copies"] += 1
                continue
            low, high = yours["ranges"].get(field, mine["ranges"].get(field, (0, 0)))
            if not low <= want[0] <= high:
                skipped["out_of_range"] += 1
                continue
            rows.append({"first_name": here["first_name"], "last_name": here["last_name"],
                         "field": field, "value": want[0]})
    skipped["missing_players"] = sum(1 for k in mine["players"] if k not in yours["players"])
    return rows, skipped


def import_receipt(ours: dict, theirs: dict, rows: list[dict], skipped: dict) -> dict:
    """Name the import, so a room refusal can say whose roster this is.

    Digests rather than a version string: the interesting case is a Reborn we
    have no label for, and two players can still be told whether their imports
    are the same thing. No player names or attribute values - that would make
    the receipt an exported roster by another route.
    """
    return {"format": "nsr.roster-import.v1",
            "imported_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "source": {"label": theirs["label"], "image_sha256": theirs["sha256"],
                       "roster_sha256": theirs["roster_sha256"]},
            "target": {"label": ours["label"], "image_sha256": ours["sha256"],
                       "roster_sha256": ours["roster_sha256"]},
            "counts": {"attributes": len(rows),
                       "players": len({(r["first_name"], r["last_name"]) for r in rows}),
                       **skipped}}


def _write_json(target: Path, receipt: dict) -> Path:
    """Write one receipt atomically, through eadb's local/ boundary."""
    target = eadb.local_output(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_name(target.name + ".tmp")
    tmp.write_text(json.dumps(receipt, indent=1), encoding="utf-8")
    os.replace(tmp, target)
    return target


def _editor():
    """The ratings service, imported only by the developer CLI.

    Lazy and in one place: the shipped launcher vendors this module but not the
    editor, and `plan_import`/`apply_locally` deliberately need none of it. A
    packaged build that somehow reached this path should say so plainly rather
    than raising ImportError from the middle of an import.
    """
    try:
        import core
    except ImportError as exc:  # pragma: no cover - not reachable from the launcher
        raise RosterImportError("saving a pack needs the ratings editor, which is not part of "
                                "the installed launcher; run this from the repository") from exc
    return core


def write_receipt(directory: Path, name: str, receipt: dict) -> Path:
    """Write the receipt beside its pack, under the pack's own name."""
    return _write_json(directory / (_editor().pack_name(name) + RECEIPT_SUFFIX), receipt)


def write_sidecar(game_dir: Path, receipt: dict) -> Path:
    """Write the receipt beside the image it was baked into.

    A fixed name rather than the pack's, because the reader is the launcher
    looking at a game folder it did not build and has no pack name for
    (`nsr_patcher.read_roster_import`).
    """
    return _write_json(Path(game_dir) / nsr_patcher.ROSTER_IMPORT_NAME, receipt)


def plan_import(reborn_image: str | Path, game_image: str | Path,
                roster_file: str | None = None) -> dict:
    """Everything the import decided, before anything is written.

    Reads both images, validates the rows against the roster they will be
    applied to and pins each one's original values. Separate from applying so
    the launcher can show a player what would change, and so the whole
    decision is testable without a 1.4 GB copy.

    Deliberately free of `services.ratings.core`: the editor's HTTP, headshot
    and build code is not vendored into the shipped package, and the launcher
    needs this path. The row normalisation it shares now lives in `eadb`.
    """
    theirs = identity(reborn_image, roster_file)
    ours = identity(game_image, roster_file)
    if theirs["sha256"] == ours["sha256"]:
        raise RosterImportError("that is the same image as your game copy; nothing to import")
    rows, skipped = differences(ours["roster"], theirs["roster"])
    if not rows:
        raise RosterImportError("your game copy's roster already matches that Reborn install"
                                + (f" ({skipped['added_players']} players it adds cannot be"
                                   " created by a ratings pack)" if skipped["added_players"] else ""))
    saved = eadb.normalize_edits(ours["roster"], rows, ours["sha256"])
    if not saved:
        raise RosterImportError("none of that roster's differences can be applied to your game copy")
    return {"ours": ours, "theirs": theirs, "rows": saved, "skipped": skipped,
            "report": import_receipt(ours, theirs, saved, skipped)}


def apply_locally(game_dir: str | Path, plan: dict, progress=None) -> dict:
    """Write an imported roster into the player's own game copy.

    The roster file is a fixed-size disc file, so this replaces it in a fresh
    copy of the image and swaps that in - no Reborn rebuild and no re-patching,
    which is why it costs a file copy rather than a full build. `patch_copy`
    refuses on any drift: wrong original bytes, a changed source mid-copy, or a
    replacement of a different length, and it leaves the live image untouched
    until the new one has been verified.

    The manifest's output digest is updated afterwards, which is what keeps the
    copy classified as ours (`nsr_patcher.image_status` reads `custom` from a
    recipe plus a matching digest) rather than "not built by this version". The
    receipt is written last, so a failure never leaves a claim about a roster
    the image does not have.

    Reverting is the existing Rebuild game copy button: it regenerates from
    Reborn and the import is simply not reapplied.
    """
    game_dir = Path(game_dir)
    image = game_dir / nsr_patcher.OUTPUT_NAME
    ours = plan["ours"]
    if ours["sha256"] != discpatch.sha256(image):
        raise RosterImportError("your game copy changed since this import was planned; start again")
    manifest = nsr_patcher.read_manifest(str(game_dir))
    if manifest.get("output_sha256") != ours["sha256"]:
        raise RosterImportError("your game copy and its nsr-image.json disagree; "
                                "rebuild the game copy first")
    replacement, report = eadb.apply_edits(ours["roster"], plan["rows"],
                                           base_sha256=ours["sha256"])
    if progress:
        progress(0, 1)
    staging = eadb.LOCAL / "roster-import"
    target = staging / nsr_patcher.OUTPUT_NAME
    if target.exists():
        target.unlink()
    digest = discpatch.patch_copy(image, target, ours["roster_file"], replacement,
                                  expect=ours["roster"], expect_image_sha256=ours["sha256"])
    try:
        os.replace(target, image)
    except OSError:
        target.unlink(missing_ok=True)
        raise
    if progress:
        progress(1, 1)
    manifest["output_sha256"] = digest
    manifest["fingerprint"] = fingerprint = _fingerprint_of(manifest, digest)
    _write_manifest(game_dir, manifest)
    write_sidecar(game_dir, plan["report"])
    return {"image": str(image), "sha256": digest, "fingerprint": fingerprint,
            "changes": report}


def _fingerprint_of(manifest: dict, digest: str) -> str:
    """Fingerprint this image the way `nsr_patcher.image_status` will read it.

    The owned family carries its variant in the fingerprint, so using the
    Reborn-family form for an own-disc copy would write a value that disagrees
    with the one rooms actually compare.
    """
    if manifest.get("base_family") == nsr_patcher.ownimage_build.FAMILY:
        return nsr_patcher.retail_fingerprint(digest, manifest.get("variant", ""))
    return nsr_patcher.fingerprint(digest)


def _write_manifest(game_dir: Path, manifest: dict) -> Path:
    """Replace nsr-image.json atomically, outside eadb's local/ boundary.

    That boundary exists to keep generated data out of source trees; this file
    belongs to the player's game folder, which is the one place it has to be.
    """
    target = game_dir / nsr_patcher.MANIFEST_NAME
    tmp = target.with_name(target.name + ".tmp")
    tmp.write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    os.replace(tmp, target)
    return target


def run_import(reborn: str | None, image: str, name: str, packs: Path = PACKS,
               roster_file: str | None = None, build: bool = False) -> dict:
    """Read both rosters, save the pack and its receipt, optionally bake.

    The target image is read through the ratings editor's own loader so the pack
    is validated against exactly the image it will be applied to, including the
    netplay-hook check that rejects an unpatched disc.
    """
    plan = plan_import(find_reborn(reborn), image, roster_file)
    app = _editor().Ratings(Path(image), packs, REPO / "local" / "ratings-builds",
                            plan["ours"]["roster_file"])
    saved = app.save(name, plan["rows"])
    receipt = plan["report"]
    receipt_path = write_receipt(packs, name, receipt)
    result = {"pack": name, "receipt": str(receipt_path), "rows": saved, "report": receipt}
    if build:
        result["build"] = app.build(name)
        # The receipt travels with the image it was baked into, because that is
        # what the launcher has in hand when a room asks whose roster this is.
        # Written after the image so a failed build leaves no claim behind.
        result["sidecar"] = str(write_sidecar(Path(result["build"]["image"]).parent, receipt))
    return result


def _print_report(result: dict, sample: int = 10) -> None:
    """Report counts and a short sample. Never the roster - that is the rule."""
    receipt, rows = result["report"], result["rows"]
    counts = receipt["counts"]
    source = receipt["source"]
    print(f"imported from {source['label'] or 'an unrecognized Reborn build'} "
          f"({source['image_sha256'][:12]})")
    print(f"  {counts['attributes']} attributes on {counts['players']} players")
    for key, text in (("added_players", "players Reborn adds (a pack cannot create players)"),
                      ("inconsistent_copies", "attributes whose Reborn team copies disagree"),
                      ("out_of_range", "attributes outside the range a build accepts"),
                      ("missing_players", "players in your copy that Reborn no longer has")):
        if counts[key]:
            print(f"  skipped: {counts[key]} {text}")
    for row in rows[:sample]:
        print(f"    {row['first_name']} {row['last_name']}: {row['field']} "
              f"{row['expected']} -> {row['value']}")
    if len(rows) > sample:
        print(f"    ... and {len(rows) - sample} more")
    print(f"  pack: {result['pack']}")
    print(f"  receipt: {result['receipt']}")
    if "build" in result:
        print(f"  built: {result['build']['image']}")
        print(f"  receipt beside it: {result['sidecar']}")
        print(f"  fingerprint: {result['build']['fingerprint']} - everyone in a room "
              "needs this same import")
    else:
        print("  apply it with --build, or from the ratings editor; everyone in a room "
              "needs the same import")


def main(argv=None) -> int:
    """Two commands: look without writing, or import."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reborn", help="your Reborn folder or its NSR.iso (default: search)")
    ap.add_argument("--image", required=True, help="your configured nsr-net.iso")
    ap.add_argument("--roster-file", help="exact FST path of the roster file, if not template.dat")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan", help="report what both images are, and write nothing")
    imp = sub.add_parser("import", help="save a ratings pack from the difference")
    imp.add_argument("--name", default="Reborn roster", help="pack name")
    imp.add_argument("--packs", default=str(PACKS), help="pack directory under local/")
    imp.add_argument("--build", action="store_true",
                     help="also bake a new image from the pack (a full rebuild)")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "scan":
            theirs = identity(find_reborn(a.reborn), a.roster_file)
            ours = identity(a.image, a.roster_file)
            # Not plan_import: a scan reports "nothing differs" rather than
            # refusing, which is a useful answer and the common one.
            rows, skipped = differences(ours["roster"], theirs["roster"])
            for which, info in (("yours", ours), ("Reborn", theirs)):
                print(f"{which}: {info['label'] or 'unrecognized build'} "
                      f"{info['sha256'][:12]} roster {info['roster_sha256'][:12]}")
            print(f"{len(rows)} attributes differ on "
                  f"{len({(r['first_name'], r['last_name']) for r in rows})} players")
            for key, value in skipped.items():
                if value:
                    print(f"  {key}: {value}")
            return 0
        _print_report(run_import(a.reborn, a.image, a.name, Path(a.packs),
                                 a.roster_file, a.build))
        return 0
    except (RosterImportError, ValueError, OSError, discpatch.PatchError) as exc:
        print(f"roster import: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
