"""Applying a signed application update: download, stage, swap, revert.

`nsr_update` decides *whether* to update and what a manifest may say; this
module is the half that touches the disk. They are deliberately separate
files. `docs/autoupdate.md` put this code "in the launcher", but the launcher
is a single 136 KB GUI class, and the code that renames a running executable
and moves files over a player's install is the last code in this project that
should be hard to test. So the mechanics live here, where they run headless
under pytest, and the launcher keeps only the policy: when to check, what to
tell the player, and when to defer.

**Install model.** Files are replaced in place, with the previous copy of
every replaced file retained so a failure can be undone. `release-plan.md`
§4.2 describes immutable version directories with a `current.json` pointer,
and `docs/autoupdate.md` step 2 instead describes renaming the running
executable aside - those are different models, and this is the second one.
The reason is the package layout: it is a portable install where the player's
`nsr-net.iso`, `User/`, `GameSources/`, `Assets/` and texture packs sit in the
same folder as the executable. Version directories would mean relocating or
indirecting mutable player data on every update, which is a larger change and
a worse risk than the one it avoids. The deviation is recorded in
`docs/autoupdate.md`.

**What is never touched.** Only paths the signed manifest lists are read,
replaced or restored. Everything else in the folder is the player's: the game
image above all, which an update must never download, delete or rebuild
behind their back. `nsr_update.plan()` reports unlisted local files as
`stale`; nothing here acts on that list.

**Crash safety.** A swap journals its intent before it moves anything, so an
interrupted update is a recognisable state rather than a silent mixture. The
next start finds the journal and can finish or undo it. Separately, a new
build is on trial until it says so itself: the launcher confirms a successful
start, and a build that never gets that far is reverted on the following
start rather than leaving a player stuck on something that does not run.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import urllib.error
import urllib.request

import nsr_content as content
import nsr_update as update
from nsr_content import ContentError

# Beside the launcher, like `nsr-content.txt`: the release channel's base URL.
# Absent, the download simply has no update channel and play is unaffected.
CHANNEL_FILE = "nsr-build.txt"
MANIFEST_NAME = "nsr-build-manifest.json"

# Release files are fetched by content hash from one flat namespace, never by
# their install path and never under a prefix. The reason is the release host:
# Mason's decision is GitHub Releases, which serves
# `/releases/latest/download/<asset>` where <asset> is a single file name - so
# neither `Sys/totaldb.dsy` nor `files/<hash>` is addressable there. A static
# host serves a flat directory just as happily, so flat costs nothing and is
# the only layout that works on both. `nsr_content` already distributes its
# packs by hash for the same reason.
#
# Within one package, two identical files collapse to one asset. Across
# releases they do not, when the host is GitHub Releases: `latest/download`
# only sees the newest release's assets, so every release carries its full
# set. Clients still download only what changed - the upload is Mason's cost,
# the download is his friends'.
#
# The manifest still carries each install path. That is where the bytes go and
# it is validated against Windows' rules; the hash decides only what to ask
# the host for.
def file_url(base: str, sha256: str) -> str:
    """Where one release file lives: flat, content-addressed, hostable."""
    return f"{base.rstrip('/')}/{sha256}"

# The installed build's identity. A fresh zip install has no such file, which
# reads as version 0 - see `installed()` for why that is safe rather than a
# reason to re-download everything.
RECORD_NAME = "nsr-build.json"

# Working directories, all inside the install folder so a half-finished update
# cannot be left on another volume where the final move would not be atomic.
# The control files live apart from the retained originals on purpose:
# `ROLLBACK_DIR` mirrors the manifest's own layout, so anything of ours kept
# inside it would share a namespace with a shipped file - a release listing a
# root-level `journal.json` would have collided with the journal and been
# skipped by the restore.
STAGING_DIR = ".nsr-staging"
ROLLBACK_DIR = ".nsr-rollback"
CONTROL_DIR = ".nsr-update"
JOURNAL_NAME = "journal.json"
TRIAL_NAME = "trial.json"

# A single file's download cap. `nsr_update.MAX_FILE_BYTES` is 256 MB, which is
# the manifest's limit; this is the transfer's, and it streams to disk, so
# memory stays flat regardless.
CHUNK = 1 << 20
FETCH_TIMEOUT = 30.0


class UpdateError(RuntimeError):
    """A failure that leaves the install as it was. The message is for a player."""


# -- where updates come from, and what is installed ---------------------------

def channel_url(folder: str, env=os.environ) -> str:
    """The release channel base URL, or "" when this download has no channel.

    `NSR_BUILD_URL` overrides the shipped file, which is what the tests and a
    development release use. Both go through `check_base_url`, so an http URL
    is refused unless it is loopback: an unsigned manifest is already refused,
    but there is no reason to fetch one over plaintext from the internet.
    """
    override = (env.get("NSR_BUILD_URL") or "").strip()
    if override:
        return content.check_base_url(override)
    try:
        with open(os.path.join(folder, CHANNEL_FILE), encoding="utf-8") as fh:
            return content.check_base_url(fh.read().strip())
    except OSError:
        return ""


def installed(folder: str) -> dict:
    """The installed build record: {version, label, recipe, serial}.

    A missing or unreadable record reads as version 0 with no recipe, and that
    is deliberately safe rather than a trigger to re-download. A player who
    unzipped a fresh package has no record, but `plan()` compares the
    manifest's files against what is on disk, so the identical build produces
    an empty fetch list and the launcher simply records the version it already
    has. The record is an ordering guard and a cheap fast path, not the
    authority on what is installed; the files are.
    """
    try:
        with open(os.path.join(folder, RECORD_NAME), encoding="utf-8") as fh:
            data = json.load(fh)
        if not isinstance(data, dict):
            raise ValueError("not an object")
        return {"version": int(data.get("version", 0) or 0),
                "label": str(data.get("label", "") or ""),
                "recipe": str(data.get("recipe", "") or ""),
                "serial": int(data.get("serial", 0) or 0)}
    except (OSError, ValueError, TypeError):
        return {"version": 0, "label": "", "recipe": "", "serial": 0}


def write_record(folder: str, build: dict, serial: int) -> None:
    """Record the installed build. Atomic: a torn record would read as 0."""
    content._atomic_write(os.path.join(folder, RECORD_NAME), json.dumps(
        {"version": build["version"], "label": build["label"],
         "recipe": build["recipe"], "serial": int(serial)}, indent=1).encode("utf-8"))


def installed_recipe(game_dir: str, patcher_mod=None) -> str:
    """The recipe the player's *image* was actually built with.

    Taken from the image's own `nsr-image.json` (via the patcher, which owns
    that file) rather than from our build record, because the builder writes
    it and it is the ground truth for what the image is. Our record can be
    missing - a player who just unzipped a package has none - or stale, and
    either would otherwise read as "the recipe changed" and ask them to
    rebuild a perfectly good 1.4 GB image for nothing.

    "" when there is no image yet, which is not a rebuild: there is nothing
    to rebuild, and the first build will use the current recipe anyway.
    """
    if patcher_mod is None:
        import nsr_patcher as patcher_mod
    return str(patcher_mod.read_manifest(game_dir).get("recipe", "") or "")


# -- fetching -----------------------------------------------------------------

def needs_rebuild(folder: str, game_dir: str, patcher_mod=None) -> bool:
    """Does the installed image predate the installed build's recipe?

    This is how the image rebuild is sequenced, and it needs no marker file:
    after a build that changed the recipe is installed, the build record says
    one recipe and the image's own `nsr-image.json` says another, and that
    disagreement *is* the signal.

    The rebuild deliberately happens on the next start, under the new build,
    rather than before the relaunch. The recipe belongs to the new code - the
    old launcher rebuilding at the new recipe's request would produce an image
    from the old patcher, which is the one combination nobody has qualified.

    False when there is no image: nothing to rebuild, and the first build will
    use the current recipe anyway.
    """
    wanted = installed(folder)["recipe"]
    if not wanted:
        return False
    current = installed_recipe(game_dir, patcher_mod)
    return bool(current) and current != wanted


def _open(url: str, timeout: float):
    req = urllib.request.Request(url, headers={
        "User-Agent": "NFLStreetRemastered-update/1", "Accept": "application/octet-stream"})
    return urllib.request.urlopen(req, timeout=timeout)  # noqa: S310 (scheme checked by caller)


def fetch_to_file(url: str, dest: str, *, size: int, sha256: str, opener=_open,
                  timeout: float = FETCH_TIMEOUT) -> None:
    """Stream one file to `dest`, hashing as it goes; raise unless it matches.

    Streaming rather than `nsr_content.http_get` because that returns bytes,
    and a release file can be 22 MB (`Dolphin.exe`) against a manifest limit
    of 256 MB. Holding that in memory on a friend's PC to then write it out
    has no upside.

    The size cap is the manifest's own figure for this file, so a server that
    keeps sending cannot fill the disk, and the hash is checked here as well
    as in `verify_staged` - once while the bytes are in hand, once over the
    whole staging directory - because a file that fails now should not be left
    on disk looking plausible.
    """
    digest, written = hashlib.sha256(), 0
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    try:
        with opener(url, timeout) as response, open(dest, "wb") as out:
            while chunk := response.read(CHUNK):
                written += len(chunk)
                if written > size:
                    raise UpdateError(f"{url} sent more than its manifest size")
                digest.update(chunk)
                out.write(chunk)
    except urllib.error.HTTPError as exc:
        raise OSError(f"HTTP {exc.code}") from None
    except (urllib.error.URLError, TimeoutError, ValueError) as exc:
        raise OSError(str(getattr(exc, "reason", exc))) from None
    if written != size or digest.hexdigest() != sha256:
        raise UpdateError(f"{os.path.basename(dest)} did not match the signed manifest")


def fetch_manifest(base: str, fetch=None, keys=None) -> tuple[dict, bytes]:
    """Fetch and verify the channel's signed manifest. (payload, raw bytes)."""
    fetch = fetch or content.http_get
    raw = fetch(base.rstrip("/") + "/" + MANIFEST_NAME, update.MAX_MANIFEST_BYTES)
    return update.verify(raw, keys=keys), raw


# -- staging ------------------------------------------------------------------

def _dest(root: str, rel: str) -> str:
    return os.path.join(root, *rel.split("/"))


def stage(payload: dict, plan: update.Plan, install_root: str, base: str, *,
          staging: str | None = None, fetch=fetch_to_file, reuse=True,
          progress=None) -> str:
    """Build a complete, verified copy of the new build in a staging folder.

    Every file the manifest lists ends up here, not only the downloaded ones:
    the ones already correct locally are copied (or hard-linked) across, so
    the staging directory can be checked as a whole before anything in the
    install is touched. `verify_staged` with no `only` is what makes that
    check meaningful - a half-downloaded staging folder cannot pass it.

    Returns the staging path. Raises `UpdateError` or `OSError` without having
    modified the install.
    """
    build = payload["build"]
    staging = staging or os.path.join(install_root, STAGING_DIR)
    # A staging folder left by an interrupted attempt tells us nothing we can
    # trust, and its files are cheap to fetch again compared to the risk.
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging, exist_ok=True)

    wanted = {f["path"]: f for f in build["files"]}
    for index, rel in enumerate(plan.fetch, 1):
        entry = wanted[rel]
        if progress:
            progress(index, len(plan.fetch), rel)
        fetch(file_url(base, entry["sha256"]), _dest(staging, rel),
              size=entry["size"], sha256=entry["sha256"])
    if reuse:
        for rel in plan.reuse:
            src, dst = _dest(install_root, rel), _dest(staging, rel)
            os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
            try:  # a hard link costs nothing; copying is the fallback
                os.link(src, dst)
            except (OSError, NotImplementedError):
                shutil.copy2(src, dst)

    bad = update.verify_staged(payload, staging)
    if bad:
        raise UpdateError("The update's files didn't verify, so nothing was changed "
                          f"({len(bad)} of {len(build['files'])} wrong)")
    return staging


# -- the swap -----------------------------------------------------------------

def _journal_path(install_root: str) -> str:
    return os.path.join(install_root, CONTROL_DIR, JOURNAL_NAME)


def swap_in(payload: dict, staging: str, install_root: str, *, replace=None,
            verify: bool = True) -> list[str]:
    """Move the staged build into place, keeping every replaced file.

    Order matters, and this is the reason the running executable can be
    replaced at all: each existing file is *renamed* into `ROLLBACK_DIR`
    before the new one is moved in. Windows refuses to delete or overwrite a
    running image but permits renaming it, so the launcher's own executable
    moves aside and the new one takes its path, which is what makes a
    self-replace possible without a helper binary. **That behaviour is
    asserted by this design and must still be demonstrated on Windows** - it
    is not something these tests can establish from Linux.

    `replace` is the set of paths that actually differ - `plan.fetch` - and
    only those are touched. None means every file the manifest lists, which
    is the bootstrapper's empty-folder case. Restricting it is not only
    cheaper (a launcher-only update moves two files, not the whole 92 MB
    package): moving a file that is already correct is pure risk, and it was
    also wrong. `stage()` hard-links unchanged files into staging, so moving
    one aside and then moving the staged copy in left the install path and
    the retained path as two names for one inode - and POSIX `rename()` on
    two names of the same file succeeds while doing nothing, so the restore
    silently no-opped and the retained copy was never released.

    The journal is written before the first move and removed after the last,
    so an interruption leaves a recognisable state with every original file
    still present under `ROLLBACK_DIR`. Returns the relative paths replaced.
    """
    build = payload["build"]
    if verify:
        bad = update.verify_staged(payload, staging)
        if bad:
            raise UpdateError("Refusing to install: the staged files no longer verify")
    listed = [f["path"] for f in build["files"]]
    if replace is None:
        paths = listed
    else:
        unknown = sorted(set(replace) - set(listed))
        if unknown:
            raise UpdateError(f"Refusing to install a path the manifest does not list: {unknown[0]}")
        paths = [p for p in listed if p in set(replace)]

    rollback = os.path.join(install_root, ROLLBACK_DIR)
    shutil.rmtree(rollback, ignore_errors=True)
    os.makedirs(rollback, exist_ok=True)

    content._atomic_write(_journal_path(install_root), json.dumps(
        {"version": build["version"], "label": build["label"], "paths": paths}, indent=1).encode("utf-8"))

    moved = []
    try:
        for rel in paths:
            live, new = _dest(install_root, rel), _dest(staging, rel)
            if os.path.exists(live):
                kept = _dest(rollback, rel)
                os.makedirs(os.path.dirname(kept) or ".", exist_ok=True)
                os.replace(live, kept)
            else:
                os.makedirs(os.path.dirname(live) or ".", exist_ok=True)
            os.replace(new, live)
            moved.append(rel)
    except OSError as exc:
        # Undo everything retained, not just the paths that completed. Each
        # file is renamed aside before its replacement moves in, so a failure
        # in between leaves one path with its original already in ROLLBACK_DIR
        # and nothing live - and that path is not yet in `moved`. Restoring
        # `moved` alone would have left the install without its launcher
        # executable, which is how this was found. The journal stays, so a
        # crash during the undo is still recoverable on the next start.
        _restore(install_root, _retained(install_root))
        if _retained(install_root):
            # The undo itself could not finish, so promising the previous
            # version was kept would be false. The originals are still on
            # disk and the journal still names them, so the next start's
            # `resolve_pending` will try again - say that instead.
            raise UpdateError(f"Couldn't install the update ({exc}), and restoring the previous "
                              "version didn't finish. Start the launcher again to complete it.") from None
        raise UpdateError(f"Couldn't install the update ({exc}); your previous version was kept") from None
    os.remove(_journal_path(install_root))
    shutil.rmtree(staging, ignore_errors=True)
    if not _retained(install_root):
        # A first install replaces nothing, so there is nothing to undo and no
        # trial to end. Leaving an empty rollback directory would outlive the
        # install, since only `confirm()` clears it and the bootstrapper never
        # starts a trial.
        shutil.rmtree(rollback, ignore_errors=True)
    return moved


def _restore(install_root: str, paths) -> list[str]:
    """Move retained originals back over the installed paths."""
    rollback = os.path.join(install_root, ROLLBACK_DIR)
    back = []
    for rel in paths:
        kept = _dest(rollback, rel)
        if not os.path.exists(kept):
            continue
        live = _dest(install_root, rel)
        os.makedirs(os.path.dirname(live) or ".", exist_ok=True)
        try:
            os.replace(kept, live)
            back.append(rel)
        except OSError:
            continue
    return back


def interrupted(install_root: str) -> dict | None:
    """The journal of a swap that never finished, or None."""
    try:
        with open(_journal_path(install_root), encoding="utf-8") as fh:
            data = json.load(fh)
        return data if isinstance(data, dict) and isinstance(data.get("paths"), list) else None
    except (OSError, ValueError):
        return None


def revert(install_root: str, paths=None) -> list[str]:
    """Put the previous build back. Returns the paths restored.

    Used for the two ways an update can fail after the swap: an interrupted
    swap found on the next start, and a new build that started badly enough
    never to confirm itself.
    """
    journal = interrupted(install_root)
    if paths is None:
        paths = journal["paths"] if journal else _retained(install_root)
    back = _restore(install_root, paths)
    clear_trial(install_root)
    shutil.rmtree(os.path.join(install_root, ROLLBACK_DIR), ignore_errors=True)
    try:
        os.remove(_journal_path(install_root))
    except OSError:
        pass
    return back


def _retained(install_root: str) -> list[str]:
    """Every original currently held, as relative paths. Journal or not.

    ROLLBACK_DIR holds nothing but replaced files, at their own relative
    paths, so this needs no exclusions - which is why the journal and trial
    live in CONTROL_DIR instead.
    """
    rollback = os.path.join(install_root, ROLLBACK_DIR)
    out = []
    for folder, _dirs, names in os.walk(rollback):
        for name in names:
            out.append(os.path.relpath(os.path.join(folder, name), rollback).replace(os.sep, "/"))
    return sorted(out)


def discard_previous(install_root: str) -> None:
    """Drop the retained previous build, once the new one has proved itself."""
    shutil.rmtree(os.path.join(install_root, ROLLBACK_DIR), ignore_errors=True)
    shutil.rmtree(os.path.join(install_root, CONTROL_DIR), ignore_errors=True)


# -- the trial ----------------------------------------------------------------

def _trial_path(install_root: str) -> str:
    return os.path.join(install_root, CONTROL_DIR, TRIAL_NAME)


def begin_trial(install_root: str, build: dict) -> None:
    """Mark the new build as unproven, just before relaunching into it."""
    content._atomic_write(_trial_path(install_root), json.dumps(
        {"version": build["version"], "label": build["label"], "starts": 0}, indent=1).encode("utf-8"))


def trial(install_root: str) -> dict | None:
    try:
        with open(_trial_path(install_root), encoding="utf-8") as fh:
            data = json.load(fh)
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def clear_trial(install_root: str) -> None:
    try:
        os.remove(_trial_path(install_root))
    except OSError:
        pass


def note_start(install_root: str) -> int:
    """Count this start against the pending trial; returns the count.

    A build that gets far enough to run this and then dies still burns an
    attempt, which is the point: the alternative is a crash loop that never
    reverts because it never reaches the confirmation.
    """
    pending = trial(install_root)
    if not pending:
        return 0
    pending["starts"] = int(pending.get("starts", 0) or 0) + 1
    content._atomic_write(_trial_path(install_root),
                          json.dumps(pending, indent=1).encode("utf-8"))
    return pending["starts"]


MAX_TRIAL_STARTS = 2


def resolve_pending(install_root: str) -> str:
    """Settle whatever the last run left behind. Call once, early, at startup.

    Returns what happened, for the log and the player-facing note:

    * `""` - nothing was pending.
    * `"reverted-interrupted"` - a swap had not finished; the previous build
      is back. Every original was still retained, so this is a clean undo.
    * `"reverted-failed-start"` - the new build was installed but never
      confirmed itself within `MAX_TRIAL_STARTS`; the previous build is back.
    * `"on-trial"` - a new build is installed and unproven, and this start
      counts towards its allowance. The caller confirms it when the UI is up.
    """
    if interrupted(install_root):
        revert(install_root)
        return "reverted-interrupted"
    if trial(install_root):
        if note_start(install_root) > MAX_TRIAL_STARTS:
            revert(install_root)
            return "reverted-failed-start"
        return "on-trial"
    return ""


def confirm(install_root: str) -> None:
    """The new build came up. Clear the trial and drop the previous copy."""
    clear_trial(install_root)
    discard_previous(install_root)


# -- deferral -----------------------------------------------------------------

def defer_reason(*, in_room: bool, game_running: bool, building_image: bool) -> str:
    """Why an update must wait, as something the player can act on, or "".

    An update is never allowed to interrupt play. These three are checked in
    the order that matters to the player: a live room has other people in it,
    a running game is theirs alone, and an image build must not be left
    half-done because an update wanted the folder.
    """
    if in_room:
        return "Leave the room first, then check for updates again."
    if game_running:
        return "Close the game first, then check for updates again."
    if building_image:
        return "Wait for the current game copy to finish building."
    return ""
