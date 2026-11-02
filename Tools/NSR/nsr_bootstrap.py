"""One file a friend runs once: fetch the signed release, install it, start it.

This is step 6 of `docs/autoupdate.md`, and it exists because of what Mason
actually asked for - "instead of needing to hand out copies of the zip every
time they can just start the launcher and get the latest version". The full
zip remains a supported install path; this is the one that is a single file.

It is deliberately thin. Installing into an empty folder is the same code path
as an update into a populated one - `plan()` simply reports every file as
missing, and the swap has nothing to move aside - so this module adds no
install logic of its own. It finds the channel, verifies the manifest, calls
`nsr_selfupdate`, and starts what it installed.

**It never downloads a game image, and it cannot.** A release manifest lists
the package's own files; the player's copy of the game is theirs, and the
launcher asks for it on first run. Nothing here changes that.

What it refuses to do:

* install anything whose signed manifest does not verify against the trust
  anchor compiled in from `nsr_content.TRUSTED_KEYS`;
* install over a folder whose files it has not accounted for, beyond the paths
  the manifest lists;
* carry on after a partial download - the staged build is verified whole
  before a single live file is touched.

Usage (the packaged executable takes the same arguments):

    python3 tools/nsr_bootstrap.py --channel https://builds.example/base
    python3 tools/nsr_bootstrap.py --into ./install --no-start
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import nsr_content as content  # noqa: E402
import nsr_selfupdate as selfupdate  # noqa: E402
import nsr_update as update  # noqa: E402

# A channel baked into the packaged executable, so what Mason hands out really
# is one file. PyInstaller bundles it with `--add-data`; in a checkout there is
# no such file and `--channel` or `NSR_BUILD_URL` supplies it instead.
CHANNEL_RESOURCE = "nsr-channel.txt"


def bundled_channel() -> str:
    """The channel compiled into this executable, or ""."""
    base = getattr(sys, "_MEIPASS", None) or os.path.dirname(os.path.abspath(__file__))
    try:
        with open(os.path.join(base, CHANNEL_RESOURCE), encoding="utf-8") as fh:
            return content.check_base_url(fh.read().strip())
    except OSError:
        return ""


def resolve_channel(args, folder: str, env=os.environ) -> str:
    """Where to fetch from, most explicit first.

    An empty folder has no `nsr-build.txt` to read, which is why a bundled
    channel exists at all; the file is still consulted so a bootstrapper can
    be dropped into an existing install and do the right thing.
    """
    for candidate in (args.channel, env.get("NSR_BUILD_URL", ""),
                      selfupdate.channel_url(folder, env), bundled_channel()):
        checked = content.check_base_url(candidate or "")
        if checked:
            return checked
    return ""


def _say(*parts) -> None:
    """Print and flush. A download of this size takes minutes, and buffered
    output means a player (or a log) sees nothing at all until it ends."""
    print(*parts, flush=True)


def install(folder: str, channel: str, *, keys=None, fetch=None, stream=None,
            out=_say) -> dict:
    """Install the channel's current release into `folder`. Returns the build.

    Raises `selfupdate.UpdateError` for anything that should leave the folder
    as it was, and `OSError` when the release host cannot be reached.
    """
    payload, _raw = selfupdate.fetch_manifest(channel, fetch, keys)
    build = payload["build"]
    record = selfupdate.installed(folder)
    paths = {f["path"] for f in build["files"]}
    plan = update.plan(payload, installed_version=record["version"],
                       installed_recipe=selfupdate.installed_recipe(folder),
                       have=update.inventory(folder, only=paths),
                       launcher_format=update.LAUNCHER_FORMAT,
                       last_serial=0)
    if plan.blocked and record["version"] < build["version"]:
        raise selfupdate.UpdateError(plan.blocked)
    if not plan.fetch:
        # Already complete: record it so the launcher does not ask again, and
        # start it. This is what running the bootstrapper twice looks like.
        selfupdate.write_record(folder, build, payload["serial"])
        out(f"Already up to date: {build['label']}")
        return build

    out(f"Downloading {build['label']} ({plan.bytes_to_fetch / (1 << 20):.0f} MB, "
        f"{len(plan.fetch)} files)…")
    kw = {"fetch": stream} if stream else {}
    staging = selfupdate.stage(payload, plan, folder, channel,
                               progress=lambda i, n, path: out(f"  [{i}/{n}] {path}"), **kw)
    out("Installing…")
    selfupdate.swap_in(payload, staging, folder, replace=plan.fetch)
    selfupdate.write_record(folder, build, payload["serial"])
    out(f"Installed {build['label']}.")
    return build


def start(folder: str, build: dict, popen=subprocess.Popen) -> bool:
    """Start the installed entry point. False when it could not be started."""
    entry = os.path.join(folder, *build["entry"].split("/"))
    if not os.path.isfile(entry):
        return False
    try:
        popen([entry], close_fds=True,
              creationflags=getattr(subprocess, "DETACHED_PROCESS", 0))
    except (OSError, subprocess.SubprocessError):
        return False
    return True


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Install and start NFLStreetRemastered.")
    ap.add_argument("--channel", default="",
                    help="release channel base URL (https, or http to loopback). Overrides a "
                         "compiled-in channel and nsr-build.txt.")
    ap.add_argument("--into", default="",
                    help="install folder (default: the folder this executable is in)")
    ap.add_argument("--no-start", action="store_true", help="install only; do not launch")
    a = ap.parse_args(argv)

    folder = os.path.abspath(a.into) if a.into else (
        os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False)
                                        else __file__)))
    os.makedirs(folder, exist_ok=True)
    channel = resolve_channel(a, folder)
    if not channel:
        print("No release channel is set in this download. Pass --channel https://… ,\n"
              "or ask Mason for a download with one built in.", file=sys.stderr)
        return 2
    if not content.trusted_keys():
        print("This build has no signing key compiled in, so nothing can be verified.",
              file=sys.stderr)
        return 2

    try:
        build = install(folder, channel)
    except OSError as exc:
        print(f"Couldn't reach the release host ({exc}). Nothing was changed.", file=sys.stderr)
        return 1
    except (selfupdate.UpdateError, content.ContentError) as exc:
        print(f"{exc}", file=sys.stderr)
        return 1

    if a.no_start:
        return 0
    if not start(folder, build):
        print(f"Installed. Start {build['entry']} in {folder} to play.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
