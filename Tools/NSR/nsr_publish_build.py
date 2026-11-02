"""Build and sign an application-update manifest, on Mason's machine only.

Why this is a local command and not a CI step: the signing key is the whole
trust anchor. If it lived in GitHub secrets, a GitHub compromise would be code
execution on every friend's PC and the signature would be worth nothing. So
publishing a version is deliberately two steps:

1. A tag triggers a workflow that builds the package and uploads its files as
   release assets. No signature exists yet, so no launcher will take it.
2. Mason runs this command on his own machine against those files, and uploads
   the signed manifest it writes to the same release. Only then does any client
   see the update.

The key is read from a PEM path (default the same key the content publisher
uses) and never appears in argv, an environment variable, the manifest or any
log line. `--check` does everything except sign, so a release can be inspected
before the key is touched at all.

Usage:

    python3 tools/nsr_publish_build.py \\
        --package local/work/release/NFLStreetRollback-2026.11.01 \\
        --version 7 --label 2026.11.01 \\
        --fork-commit <40 hex> --launcher-commit <40 hex> \\
        --recipe gn7e69r0.v3 --room-min-version 6 --revoke 5 \\
        --changelog local/work/release/notes.txt \\
        --out local/work/release/nsr-build-manifest.json

Nothing here reaches the network: it reads a directory, writes one file, and
leaves uploading to Mason.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import nsr_content as content  # noqa: E402
import nsr_update as update  # noqa: E402

# Default launcher-format floor. A build only needs to raise this when it uses
# manifest fields or install steps an older launcher would not understand;
# raising it needlessly strands friends on an old build with no way forward.
DEFAULT_MIN_LAUNCHER = 5


class PublishError(RuntimeError):
    pass


def collect(package: str) -> list[dict]:
    """Every shipped file under `package`, as manifest entries.

    A file the manifest format cannot describe is an error rather than a
    silent omission: shipping a package while quietly dropping one of its
    files is exactly the sort of thing that produces a build that starts and
    then fails somewhere else.
    """
    if not os.path.isdir(package):
        raise PublishError(f"package directory not found: {package}")
    entries = []
    for folder, _dirs, names in os.walk(package):
        for name in sorted(names):
            full = os.path.join(folder, name)
            rel = os.path.relpath(full, package).replace(os.sep, "/")
            if os.path.islink(full):
                raise PublishError(f"{rel} is a symlink; packages ship real files")
            try:
                update.valid_path(rel)
            except content.ContentError as e:
                raise PublishError(f"{e}; rename it or leave it out of the package") from e
            entries.append({"path": rel, "size": os.path.getsize(full),
                            "sha256": update.file_sha256(full)})
    if not entries:
        raise PublishError(f"no files found under {package}")
    return entries


def stage_assets(package: str, build: dict, out_dir: str) -> tuple[int, int]:
    """Fill `out_dir` with the release's files named by content hash.

    This is what gets uploaded. Clients fetch `<channel>/files/<sha256>`
    rather than `<channel>/<install path>`, because a release host addresses
    assets by a single flat name - a GitHub release asset name cannot contain
    "/" at all, so `Sys/totaldb.dsy` is not addressable there. `nsr_content`
    already distributes its packs this way, so this is the scheme in service
    rather than a second one.

    Laid out in shard subdirectories whose names are exactly the groups the
    client asks for (`nsr-obj-0` .. `nsr-obj-f`), so each one uploads as a
    unit. GitHub Releases allows 1000 assets per release and this package is
    ~2500 objects, which is why the shards exist at all.

    Hard-linked from the package when the filesystem allows, copied otherwise,
    so a 92 MB package does not become 184 MB on disk. Two identical files in
    one package collapse to one asset, which is counted as deduplicated.
    Empty files are skipped entirely and counted separately: a release host
    will not store a 0-byte asset, and the client writes them itself.
    """
    import hashlib
    import shutil
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nsr_selfupdate  # noqa: PLC0415
    os.makedirs(out_dir, exist_ok=True)
    staged, reused, empty = 0, 0, 0
    for entry in build["files"]:
        if entry["size"] == 0:
            # Never uploaded: a release host will not store a 0-byte asset,
            # and the client writes these itself since there is nothing to
            # transfer. Counted so the omission is visible, not silent.
            empty += 1
            continue
        src = os.path.join(package, *entry["path"].split("/"))
        shard = os.path.join(out_dir, nsr_selfupdate.shard_of(entry["sha256"]))
        os.makedirs(shard, exist_ok=True)
        dst = os.path.join(shard, entry["sha256"])
        if os.path.exists(dst):
            reused += 1
            continue
        try:
            os.link(src, dst)
        except (OSError, NotImplementedError):
            shutil.copy2(src, dst)
        # Cheap insurance: the name is a promise about the bytes, and an
        # asset whose name lies would be refused by every client after upload.
        digest = hashlib.sha256()
        with open(dst, "rb") as fh:
            while block := fh.read(1 << 20):
                digest.update(block)
        if digest.hexdigest() != entry["sha256"]:
            raise PublishError(f"{entry['path']} changed while staging the upload set")
        staged += 1
    return staged, reused, empty


def build_payload(args, entries: list[dict], issued: str) -> dict:
    changelog = ""
    if args.changelog:
        with open(args.changelog, encoding="utf-8") as fh:
            changelog = fh.read().strip()
    if not changelog:
        raise PublishError("a release needs a changelog; its first line is what players see")
    payload = {
        "format": update.MANIFEST_FORMAT,
        "channel": content.CHANNEL,
        "serial": args.serial if args.serial is not None else args.version,
        "issued": issued,
        "build": {
            "version": args.version,
            "label": args.label,
            "published": issued,
            "changelog": changelog,
            "min_launcher": args.min_launcher,
            "fork_commit": args.fork_commit,
            "launcher_commit": args.launcher_commit,
            "recipe": args.recipe,
            "room_min_version": (args.room_min_version if args.room_min_version is not None
                                 else args.version),
            "revoked": sorted(set(args.revoke)),
            "entry": args.entry,
            "files": entries,
        },
    }
    return update.validate_payload(payload)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--package", required=True, help="directory of the built package's files")
    ap.add_argument("--version", type=int, help="integer ordering version; must increase every release")
    ap.add_argument("--label", help="what players see, e.g. 2026.11.01")
    ap.add_argument("--fork-commit", help="40-hex commit of the Dolphin fork")
    ap.add_argument("--launcher-commit", help="40-hex commit of this repository")
    ap.add_argument("--recipe", help="patch-recipe identity; a change drives an image rebuild on clients")
    ap.add_argument("--changelog", help="path to the release notes")
    # The product has been renamed twice; this is the current executable, and
    # the Windows packaging guard now *fails the build* if a pre-rename name
    # (NFLStreetRollback.exe, NSR Netplay.exe) is in the package. A stale
    # default here would have produced a manifest whose entry point is not one
    # of its own files.
    ap.add_argument("--entry", default="NFLStreetRemastered.exe",
                    help="the launcher executable (default NFLStreetRemastered.exe)")
    ap.add_argument("--min-launcher", type=int, default=DEFAULT_MIN_LAUNCHER,
                    help=f"launcher-format floor (default {DEFAULT_MIN_LAUNCHER})")
    ap.add_argument("--room-min-version", type=int, default=None,
                    help="oldest version rooms accept (default: this version)")
    ap.add_argument("--revoke", type=int, action="append", default=[],
                    help="a version that must not keep running; repeatable")
    ap.add_argument("--serial", type=int, default=None,
                    help="replay-protection serial (default: the version)")
    ap.add_argument("--key", default=None,
                    help="signing key PEM (default: the content signing key)")
    ap.add_argument("--out", help="where to write the signed manifest")
    ap.add_argument("--assets", default=None,
                    help="directory to fill with the files to upload, named by content hash "
                         "(hard-linked from the package where possible). This is the upload set: "
                         "a release host addresses assets by a flat name, so the client fetches "
                         "each file as <channel>/files/<sha256>.")
    ap.add_argument("--check", action="store_true",
                    help="validate and summarise without touching the signing key")
    ap.add_argument("--stage-only", action="store_true",
                    help="only build the upload set (requires --assets), and say nothing about "
                         "a version. This is what CI runs: the objects a release serves are "
                         "named by content hash, so they can be published before anyone decides "
                         "what version they belong to - and before the manifest that names them "
                         "is signed. Nothing here needs the key.")
    args = ap.parse_args(argv)

    if args.stage_only:
        if not args.assets:
            raise PublishError("--stage-only needs --assets DIR to write the upload set into")
    else:
        missing = [f"--{n.replace('_', '-')}" for n in
                   ("version", "label", "fork_commit", "launcher_commit", "recipe",
                    "changelog", "out") if getattr(args, n) is None]
        if missing:
            raise PublishError(f"missing required options: {', '.join(missing)}")

    issued = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    entries = collect(args.package)

    if args.stage_only:
        # No version, no label, no manifest: `collect()` has already validated
        # every shipped path, and `stage_assets` only needs the file list.
        staged, reused, empty = stage_assets(args.package, {"files": entries}, args.assets)
        shards = sorted(d for d in os.listdir(args.assets)
                        if os.path.isdir(os.path.join(args.assets, d)))
        biggest = max((len(os.listdir(os.path.join(args.assets, d))) for d in shards), default=0)
        total = sum(f["size"] for f in entries)
        print(f"staged {len(entries)} files, {total / 1048576:.1f} MiB")
        print(f"  upload set: {args.assets} ({staged} objects"
              + (f", {reused} deduplicated" if reused else "")
              + (f", {empty} empty written by the client" if empty else "")
              + f", {len(shards)} shards, largest {biggest})")
        if biggest > 1000:
            raise PublishError(
                f"a shard holds {biggest} objects and a GitHub release allows 1000; "
                "raise SHARDS in nsr_selfupdate.py (it changes where clients look, so "
                "every client must be updated before a release uses the new layout)")
        return 0

    payload = build_payload(args, entries, issued)
    build = payload["build"]
    total = sum(f["size"] for f in build["files"])
    print(f"{update.describe(build)}")
    print(f"  {len(build['files'])} files, {total / 1048576:.1f} MiB")
    print(f"  recipe {build['recipe']}  fork {build['fork_commit'][:12]}  "
          f"launcher {build['launcher_commit'][:12]}")
    print(f"  serial {payload['serial']}, rooms accept {build['room_min_version']}+"
          + (f", revokes {build['revoked']}" if build["revoked"] else ""))

    if args.assets:
        staged, reused, empty = stage_assets(args.package, build, args.assets)
        shards = sorted(d for d in os.listdir(args.assets)
                        if os.path.isdir(os.path.join(args.assets, d)))
        biggest = max((len(os.listdir(os.path.join(args.assets, d))) for d in shards), default=0)
        print(f"  upload set: {args.assets} ({staged} files"
              + (f", {reused} deduplicated" if reused else "")
              + (f", {empty} empty written by the client" if empty else "")
              + f", {len(shards)} shards, largest {biggest})")
        if biggest > 1000:
            raise PublishError(
                f"a shard holds {biggest} objects and a GitHub release allows 1000; "
                "raise SHARDS in nsr_selfupdate.py (it changes where clients look, so "
                "every client must be updated before a release uses the new layout)")

    if args.check:
        print("  --check: not signed")
        return 0

    # Imported here so --check works on a machine without the signing stack.
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "services", "content"))
    import content_store  # noqa: PLC0415

    signer = content_store.Signer.load(args.key or content_store.DEFAULT_KEY)
    if signer.kid not in content.trusted_keys():
        raise PublishError(
            f"this key ({signer.kid}) is not in nsr_content.TRUSTED_KEYS, so no launcher "
            "would accept what it signs")
    raw = update.sign(payload, signer.kid, signer.sign)
    # Verify with the shipped anchor before writing, so a manifest this
    # launcher could not accept never reaches a release.
    update.verify(raw)
    with open(args.out, "wb") as fh:
        fh.write(raw)
    print(f"  signed with {signer.kid} -> {args.out} ({len(raw)} bytes)")
    print(f"  upload the files as assets, then {os.path.basename(args.out)} named exactly "
          f"{update_module_manifest_name()}")
    print("  until the signed manifest is there, no client sees the update")
    return 0


def update_module_manifest_name() -> str:
    """The asset name a client looks for. Kept in one place so the message
    cannot drift from what `nsr_selfupdate` actually fetches."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nsr_selfupdate  # noqa: PLC0415
    return nsr_selfupdate.MANIFEST_NAME


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (PublishError, content.ContentError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from None
