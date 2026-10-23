#!/usr/bin/env python3
"""nsr_textures.py: the HD texture set NFLStreetRollback's Dolphin shows, built from
the player's own NFL Street Reborn install.

Why: Reborn's game image draws placeholder art ("MISSING TEXTURES" title and menu
backgrounds, stock faces for player portraits) wherever its HD texture library
supplies the real picture, so a Dolphin without that library looks broken. The
library holds the menus, the real-player headshots (Reborn's roster tooling
writes portraits into the pristine library on its data root) and every other HD
texture; optional mod packages (footballs, team uniforms, the GameCube control
fix) and the button-icon ("prompt") packages override individual textures.

What Reborn does (read from its 3.0 install, never copied): its launcher keeps a
pristine library at <data root>/library/textures/<game id> (seeded from the
installer's content/textures), mod packages at <data root>/mods/packages and
content/{mods/packages,prompt-packages}/<id>/<version>/ (package.json schema
nsr.mod.v1, files/<game id>/tex1_*), the player's active list in activation.json
(schema nsr.activation.v1), and before each launch composes pristine + active
packages into its Dolphin user folder's Load/Textures/<game id> with hardlinks
(copies across volumes). Its controls profile picks one prompt package
(jsgme.ps-controls, jsgme.xbox-controls) or none for the GameCube art.

What we do: discover_reborn() describes the source only during import. The
resumable importer hardlinks files on the same volume, otherwise copies and
verifies size + SHA-256, into Assets/GN7E69/sets/<generation>. An atomic local
manifest publishes a complete generation. compose() reads that manifest and
returns local folders only, including either imported prompt choice. No asset
or manifest is shipped or uploaded. Reborn is never written.

Normal startup checks recorded source metadata (mtime_ns + size, directories
included for additions); it opens no Reborn assets/configs unless that metadata
changed. Missing Reborn keeps the published generation usable. Hardlinks share
an inode: Reborn editing a file in place also changes that local file; the next
metadata check reimports. Never write imported files, including during updates.

Button icons are per-PC presentation: textures never reach guest RAM in NSR
netplay (the fork answers CPU EFB peeks canonically and keeps EFB/XFB copies out
of RAM), so each peer may show its own set.

Deferred: a package that must *remove* a library texture (not replace it) is not
expressible (the fork's overlay has no masking); none of Reborn's packages do.
Revisit if Reborn ships one. Soundtrack packages are ignored (not textures).
"""
from __future__ import annotations

import hashlib
import json
import shutil
import stat
import time
from pathlib import Path
import os
import re
from dataclasses import dataclass, field

import nsr_controllers as controllers

GAME_ID = controllers.GAME_ID
LIST_DIR = "NSRTextures"           # under <User>/Load (fork HiresTextures.cpp)
MOD_SCHEMA = "nsr.mod.v1"
ACTIVATION_SCHEMA = "nsr.activation.v1"
ICON_SETS = ("gamecube", "playstation", "xbox")
ICON_CHOICES = ("auto",) + ICON_SETS
# Reborn's controls profiles: GameCube is the art the game was drawn with.
PROMPT_PACKAGES = {"playstation": "jsgme.ps-controls", "xbox": "jsgme.xbox-controls"}
ICON_LABELS = {"gamecube": "GameCube", "playstation": "PlayStation", "xbox": "Xbox"}
# Controller kinds (nsr_controllers.controller_kind) -> icon set. A generic
# gamepad is almost always an XInput-style A/B/X/Y pad; Switch Pro pads, the
# keyboard and anything unknown keep the GameCube art.
_KIND_ICONS = {"playstation": "playstation", "xbox": "xbox", "gamepad": "xbox"}
_VERSION = re.compile(r"^[0-9A-Za-z][0-9A-Za-z.+-]{0,31}$")


def icons_for_kind(kind: str | None) -> str:
    return _KIND_ICONS.get(kind or "", "gamecube")


def resolve_icons(setting: str | None, kind: str | None) -> tuple[str, str]:
    """(icon set, why): the launcher setting, or the controller kind for Auto."""
    if setting in ICON_SETS:
        return setting, "setting"
    return icons_for_kind(kind), f"auto:{kind or 'none'}"


@dataclass
class Package:
    id: str
    version: str
    folder: str                 # .../<id>/<version>/files/<game id>
    source: str                 # "data-root" | "install"


@dataclass
class TextureSet:
    base: str | None = None     # pristine library folder (.../<game id>)
    base_kind: str = "none"     # library | install | composed | none
    packages: list[Package] = field(default_factory=list)
    icons: str = "gamecube"
    icons_why: str = "auto:none"
    prompt: Package | None = None
    warnings: list[str] = field(default_factory=list)

    def folders(self) -> list[str]:
        """Lowest precedence first (the order the list file records)."""
        out = [self.base] if self.base else []
        out += [p.folder for p in self.packages]
        if self.prompt:
            out.append(self.prompt.folder)
        return out

    def summary(self) -> str:
        """One log line: what the player's Dolphin will show, and from where."""
        pk = ",".join(f"{p.id}@{p.version}" for p in self.packages) or "-"
        prompt = f"{self.prompt.id}@{self.prompt.version}" if self.prompt else "native"
        return (f"textures base={self.base_kind}:{self.base or '-'} packages={pk} "
                f"icons={self.icons} ({self.icons_why}) prompt={prompt}"
                + (f" warnings={len(self.warnings)}" if self.warnings else ""))


def _version_key(v: str):
    return [(0, int(p), "") if p.isdigit() else (1, 0, p) for p in re.split(r"[.+-]", v)]


def _nonempty_dir(path: str) -> bool:
    try:
        with os.scandir(path) as it:
            return any(True for _ in it)
    except OSError:
        return False


def _read_json(path: str):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def find_base(install: str | None, env=os.environ) -> tuple[str | None, str]:
    """The pristine HD library: the data root's library/textures/<game id> (the
    one Reborn keeps current, including roster portraits; newest wins), else the
    installer's content/textures/<game id>, else Reborn's composed tree as a
    last resort (it may carry Reborn's own button icons)."""
    best = None
    for i, root in enumerate(controllers.data_roots(install, env)):
        lib = os.path.join(root, "library", "textures", GAME_ID)
        if _nonempty_dir(lib):
            try:
                key = (os.path.getmtime(lib), -i)
            except OSError:
                continue
            if best is None or key > best[0]:
                best = (key, lib)
    if best:
        return best[1], "library"
    if install:
        seed = os.path.join(install, "content", "textures", GAME_ID)
        if _nonempty_dir(seed):
            return seed, "install"
    composed = controllers.reborn_load_path(install, env) if install else None
    if composed:
        tex = os.path.join(composed, "Textures", GAME_ID)
        if os.path.basename(composed) == "Load" and _nonempty_dir(tex):
            return tex, "composed"
    return None, "none"


def package_roots(install: str | None, env=os.environ) -> list[tuple[str, str]]:
    """Folders holding <id>/<version>/package.json, data root first (it is the
    one Reborn's updater refreshes), then the install's shipped copies."""
    roots = [(os.path.join(r, "mods", "packages"), "data-root")
             for r in controllers.data_roots(install, env) if r != install]
    if install:
        roots += [(os.path.join(install, "content", "mods", "packages"), "install"),
                  (os.path.join(install, "content", "prompt-packages"), "install")]
    return [(r, s) for r, s in roots if os.path.isdir(r)]


def find_packages(install: str | None, env=os.environ) -> dict[str, dict[str, Package]]:
    """Every usable texture package: {id: {version: Package}}. Usable = a
    nsr.mod.v1 package.json naming this id, targeting our game, with a
    files/<game id> folder. First root wins for a given id+version."""
    found: dict[str, dict[str, Package]] = {}
    for root, source in package_roots(install, env):
        try:
            ids = sorted(os.listdir(root))
        except OSError:
            continue
        for pid in ids:
            try:
                versions = sorted(os.listdir(os.path.join(root, pid)))
            except OSError:
                continue
            for ver in versions:
                if not _VERSION.match(ver) or ver in found.get(pid, {}):
                    continue
                base = os.path.join(root, pid, ver)
                meta = _read_json(os.path.join(base, "package.json"))
                folder = os.path.join(base, "files", GAME_ID)
                if (isinstance(meta, dict) and meta.get("schema") == MOD_SCHEMA
                        and meta.get("id") == pid and GAME_ID in (meta.get("targets") or [])
                        and os.path.isdir(folder)):
                    found.setdefault(pid, {})[ver] = Package(pid, ver, folder, source)
    return found


def newest(versions: dict[str, Package]) -> Package | None:
    return versions[max(versions, key=_version_key)] if versions else None


def read_activation(install: str | None, env=os.environ) -> tuple[list[tuple[str, str | None]], str]:
    """The packages the player turned on in Reborn, in Reborn's order, from the
    newest activation.json in a data root (its location inside the root is not
    documented, so the root, config/ and mods/ are checked). Field names are
    read leniently: any object with a string "id" (and optional "version")."""
    best = None
    for root in controllers.data_roots(install, env):
        for rel in ("activation.json", os.path.join("config", "activation.json"),
                    os.path.join("mods", "activation.json")):
            path = os.path.join(root, rel)
            try:
                mtime = os.path.getmtime(path)
            except OSError:
                continue
            if best is None or mtime > best[0]:
                best = (mtime, path)
    if not best:
        return [], ""
    data = _read_json(best[1])
    if not isinstance(data, dict) or data.get("schema", ACTIVATION_SCHEMA) != ACTIVATION_SCHEMA:
        return [], best[1]
    active: list[tuple[str, str | None]] = []

    def walk(node):
        if isinstance(node, dict):
            if isinstance(node.get("id"), str):
                ver = node.get("version")
                active.append((node["id"], ver if isinstance(ver, str) else None))
                return
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    return list(dict.fromkeys(active)), best[1]


def discover_reborn(install: str | None, icons_setting: str | None, pad_kind: str | None,
            env=os.environ) -> TextureSet:
    """Describe Reborn sources during import only. Never writes to Reborn."""
    ts = TextureSet()
    ts.icons, ts.icons_why = resolve_icons(icons_setting, pad_kind)
    try:
        ts.base, ts.base_kind = find_base(install, env)
        if not ts.base:
            ts.warnings.append("Reborn's HD textures were not found, so menus will show "
                               "\"MISSING TEXTURES\" art and players will have stock faces. "
                               "Pick your Reborn NSR.iso in Settings → Game copy.")
        elif ts.base_kind == "composed":
            ts.warnings.append("Using Reborn's last composed texture folder; it may carry "
                               "Reborn's own button icons.")
        packages = find_packages(install, env)
        active, _ = read_activation(install, env)
        prompt_ids = set(PROMPT_PACKAGES.values())
        for pid, ver in active:
            if pid in prompt_ids:
                continue  # the icon choice below decides
            versions = packages.get(pid, {})
            pkg = versions.get(ver) if ver else newest(versions)
            if pkg is None:
                ts.warnings.append(f"Reborn mod {pid}{'@' + ver if ver else ''} is turned on "
                                   "but its files were not found; skipped.")
                continue
            if pkg not in ts.packages:
                ts.packages.append(pkg)
        want = PROMPT_PACKAGES.get(ts.icons)
        if want:
            ts.prompt = newest(packages.get(want, {}))
            if ts.prompt is None:
                ts.warnings.append(f"{ICON_LABELS[ts.icons]} button icons ({want}) were not "
                                   "found in your Reborn install; showing GameCube icons.")
                ts.icons_why += ":missing"
                ts.icons = "gamecube"
    except Exception as e:  # noqa: BLE001 - textures must never block a launch
        ts.warnings.append(f"texture lookup failed: {e!r}")
    return ts


def list_path(our_user_dir: str) -> str:
    return os.path.join(our_user_dir, "Load", LIST_DIR, f"{GAME_ID}.txt")


def write_list(our_user_dir: str, ts: TextureSet) -> str | None:
    """Record ts for the fork (<User>/Load/NSRTextures/<game id>.txt, UTF-8, one
    folder per line, lowest precedence first). No folders: remove the list, so
    a stale one never points at an old install. Returns the path written."""
    path = list_path(our_user_dir)
    folders = ts.folders()
    if not folders:
        try:
            os.remove(path)
        except OSError:
            pass
        return None
    os.makedirs(os.path.dirname(path), exist_ok=True)
    text = ("# Written by NFLStreetRollback before each launch; lowest precedence first.\n"
            f"# {ts.summary()}\n" + "".join(os.path.abspath(f) + "\n" for f in folders))
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)
    return path


# --- private, per-player asset store ------------------------------------------
STORE_SCHEMA = "nsrb.local-assets.v1"
STORE_REL = os.path.join("Assets", GAME_ID)
SPACE_RESERVE = 16 << 20  # manifest, filesystem metadata and copy temporary file


class ImportError(OSError):
    """Actionable local import failure; never contains any asset bytes."""


def _stamp(path: str) -> list[int] | None:
    """Metadata-only update detector, including inode replacement on equal mtimes."""
    try:
        s = os.stat(path)
        return [s.st_size, s.st_mtime_ns, s.st_ino, s.st_dev]
    except FileNotFoundError:
        return None


def _digest(path: Path) -> str:
    """Stream a private file locally; hashes and bytes never leave this module."""
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def _atomic_json(path: Path, data: dict) -> None:
    """Publish metadata only after the generation is complete (same filesystem)."""
    tmp = path.with_suffix(".tmp")
    with tmp.open("w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=True)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def _local_path(root: Path, relative: str) -> Path:
    """Constrain local manifest paths; reject traversal, symlinks and line breaks."""
    if not isinstance(relative, str) or any(c in relative for c in "\r\n\\"):
        raise ValueError("Invalid local asset path")
    p = root / relative
    if (Path(relative).is_absolute() or ".." in Path(relative).parts
            or not p.resolve().is_relative_to(root.resolve())):
        raise ValueError("Local asset path escapes Assets")
    return p


def read_store(folder: str, *, validate: bool = True) -> dict:
    """Read only local metadata; validate file presence/sizes before trusting it."""
    root = Path(folder) / STORE_REL
    m = _read_json(str(root / "manifest.json"))
    try:
        if not isinstance(m, dict) or m.get("schema") != STORE_SCHEMA or not m["files"]:
            return {}
        if not isinstance(m["install"], str) or not isinstance(m["probes"], dict):
            return {}
        if not isinstance(m["bytes"], int) or not isinstance(m["imported"], str) or not isinstance(m["version"], str):
            return {}
        if not any(layer["kind"] == "base" for layer in m["layers"]):
            return {}
        for layer in m["layers"]:
            if layer["kind"] not in ("base", "mod", "prompt"):
                return {}
            if not all(isinstance(layer[k], str) for k in ("id", "version", "local")):
                return {}
            if not _local_path(root, layer["local"]).is_dir():
                return {}
        for f in m["files"]:
            if f["method"] not in ("copied", "hardlinked") or not isinstance(f["size"], int):
                return {}
            if not isinstance(f["source"], str) or not isinstance(f["stamp"], list):
                return {}
            if not isinstance(f["sha256"], str) or (f["method"] == "copied" and len(f["sha256"]) != 64):
                return {}
            p = _local_path(root, f["local"])
            if validate:
                s = p.stat()
                if not stat.S_ISREG(s.st_mode) or s.st_size != f["size"]:
                    return {}
        return m
    except (OSError, KeyError, TypeError, ValueError):
        return {}


def _source_changed(m: dict) -> bool:
    """Only stat recorded paths. Uninstalled/offline Reborn never invalidates us."""
    if not os.path.isdir(m["install"]):
        return False
    return any(_stamp(p) != value for p, value in m["probes"].items())


def _inventory(install: str, env, version: str) -> dict:
    """Discover source layers and watch paths only on first/changed/forced import.

    Import both prompt sets so a controller/icon switch never needs Reborn.
    Watch package directories/metadata, selected files and absent library roots
    to catch replacements, nested additions, activation and library migrations.
    """
    ts = discover_reborn(install, "gamecube", None, env)
    if not ts.base:
        raise ImportError("Reborn's HD texture library was not found. Locate your Reborn install to import it.")
    layers = [{"kind": "base", "folder": ts.base, "name": "base", "version": "",
               "id": "", "base_kind": ts.base_kind}]
    for pkg in ts.packages:
        layers.append({"kind": "mod", "folder": pkg.folder, "name": f"mods/{pkg.id}",
                       "id": pkg.id, "version": pkg.version})
    packages = find_packages(install, env)
    for pid in PROMPT_PACKAGES.values():
        pkg = newest(packages.get(pid, {}))
        if pkg:
            layers.append({"kind": "prompt", "folder": pkg.folder, "name": f"prompts/{pkg.id}",
                           "id": pkg.id, "version": pkg.version})
    probes = {}

    def watch(path):
        probes[str(path)] = _stamp(str(path))

    roots = controllers.data_roots(install, env)
    for root in roots:
        for rel in ("library", "library/textures", f"library/textures/{GAME_ID}",
                    "content/textures", f"content/textures/{GAME_ID}", "mods", "mods/packages",
                    "content/mods/packages", "content/prompt-packages", "activation.json",
                    "config/activation.json", "mods/activation.json", "config/settings.json",
                    "settings.json", "version.json", "version.txt", "VERSION.txt",
                    "reborn-launcher.exe", "content/manifest.json", "content/game-patch/chain.json",
                    "content/games/NSR.iso"):
            watch(Path(root) / rel)
    # Missing alternate data roots must also trigger migration from installer seed.
    for var in ("APPDATA", "LOCALAPPDATA"):
        if env.get(var):
            for name in ("NFL Street Reborn", "com.nflstreetreborn.launcher", "nfl-street-reborn", "nflstreetreborn"):
                watch(Path(env[var]) / name / "library" / "textures" / GAME_ID)
    for root, _ in package_roots(install, env):
        for directory, dirs, files in os.walk(root, followlinks=False):
            # Package catalog only: texture subtrees are watched below if selected.
            dirs[:] = [d for d in dirs if d != "files" and not Path(directory, d).is_symlink()]
            watch(directory)
            if "package.json" in files:
                watch(Path(directory) / "package.json")
    # A human-readable version where available; otherwise the image's known label
    # or "unknown". Every candidate remains watched even when it is absent.
    chain_path = Path(install) / "content" / "game-patch" / "chain.json"
    if chain_path.is_file() and chain_path.stat().st_size < 65536:
        chain = _read_json(str(chain_path))
        if isinstance(chain, dict) and chain.get("disc_id") == GAME_ID:
            steps = chain.get("steps")
            if isinstance(steps, list) and steps and isinstance(steps[-1], dict):
                label = steps[-1].get("label")
                if isinstance(label, str) and label:
                    version = label.replace("\n", " ").replace("\r", " ")[:128]
    for name in ("VERSION.txt", "version.txt", "version.json"):
        path = Path(install) / name
        if path.is_file() and path.stat().st_size < 4096:
            value = path.read_text(encoding="utf-8-sig").strip()
            if name.endswith(".json"):
                try:
                    value = json.loads(value).get("version", "")
                except (ValueError, AttributeError):
                    value = ""
            if isinstance(value, str) and value:
                version = value.replace("\n", " ").replace("\r", " ")[:128]
                break
    files = []
    for layer in layers:
        # IDs come from directory names but still reject paths unsafe on Windows.
        if layer["id"] and not _VERSION.fullmatch(layer["id"]):
            raise ImportError("Reborn contains an unsupported texture package name.")
        source = Path(layer["folder"])
        for directory, dirs, names in os.walk(source, followlinks=False):
            if any(Path(directory, d).is_symlink() for d in dirs):
                raise ImportError("Reborn texture folders must not contain symbolic links.")
            dirs.sort()
            watch(directory)
            for name in sorted(names):
                p = Path(directory) / name
                if p.is_symlink() or not p.is_file():
                    raise ImportError("Reborn textures must be regular local files.")
                watch(p)
                rel = (Path(layer["name"]) / p.relative_to(source)).as_posix()
                files.append({"source": str(p), "relative": rel, "size": p.stat().st_size,
                              "stamp": probes[str(p)]})
    if not files:
        raise ImportError("Reborn's HD texture library is empty; no assets were imported.")
    return {"install": os.path.abspath(install), "version": version or "unknown",
            "layers": layers, "files": files, "probes": probes, "warnings": ts.warnings}


def _check_space(path: Path, required: int) -> None:
    """Leave room for remaining copies and metadata; resumable work is retained."""
    free = shutil.disk_usage(path).free
    if free < required:
        raise ImportError(f"Not enough disk space to import Reborn textures: need "
                          f"{required / (1 << 20):.1f} MB free, have {free / (1 << 20):.1f} MB. "
                          "Free space in the NFLStreetRollback drive and click Re-import from Reborn. "
                          "Completed files will be resumed; your existing game copy is kept.")


def ensure_store(folder: str, install: str | None, env=os.environ, *, force: bool = False,
                 version: str = "unknown", progress=None) -> dict:
    """Import locally if needed; resume verified files after interruption.

    Old generations stay intact on failure. No automatic deletion: future disk
    reclamation needs an explicit player action, especially with multiple users.
    Only this function writes Assets; callers serialize imports against gameplay.
    """
    root = Path(folder) / STORE_REL
    old = read_store(folder)
    if old and not force and (not install or os.path.abspath(install) == old["install"]) and not _source_changed(old):
        return old
    install = install or old.get("install")
    if not install or not os.path.isdir(install):
        if old and not force:
            return old
        raise ImportError("Reborn is needed for this import. Your existing local game and textures are kept.")
    if any(root.resolve().is_relative_to(Path(source_root).resolve())
           for source_root in controllers.data_roots(install, env)):
        raise ImportError("Keep NFLStreetRollback in its own folder outside Reborn before importing.")
    plan = _inventory(install, env, version)
    if force:
        # Explicit re-import catches same-size edits with preserved timestamps.
        # Include hashes in the generation ID, so repair never rewrites the
        # published generation and a second attempt can still resume.
        plan["forced_sha256"] = {f["source"]: _digest(Path(f["source"])) for f in plan["files"]}
    generation = hashlib.sha256(json.dumps(plan, sort_keys=True).encode()).hexdigest()[:24]
    dest = root / "sets" / generation
    # Hardlinks still need directory entries, a journal and an atomic manifest.
    # Check the nearest existing destination ancestor before creating anything.
    ancestor = root
    while not ancestor.exists():
        ancestor = ancestor.parent
    _check_space(ancestor, SPACE_RESERVE)
    dest.mkdir(parents=True, exist_ok=True)
    journal = dest / "completed.jsonl"
    completed = {}
    try:
        for line in journal.read_text(encoding="utf-8").splitlines():
            try:
                entry = json.loads(line)
                completed[entry["relative"]] = entry
            except (ValueError, KeyError):
                continue  # a torn final journal record is simply retried
    except FileNotFoundError:
        pass
    total = sum(f["size"] for f in plan["files"])
    done = 0
    report = progress or (lambda d, t: None)
    report(0, total)
    records = []
    old_files = {f["source"]: f for f in old.get("files", [])}
    remaining = sum(f["size"] for f in plan["files"]
                    if old_files.get(f["source"], {}).get("stamp") != f["stamp"]
                    and completed.get(f["relative"], {}).get("stamp") != f["stamp"])
    with journal.open("a", encoding="utf-8") as log:
        log.write("\n")  # separate any torn last record before appending new ones
        for f in plan["files"]:
            out = _local_path(dest, f["relative"])
            out.parent.mkdir(parents=True, exist_ok=True)
            previous = completed.get(f["relative"], {})
            valid = False
            if out.is_file() and out.stat().st_size == f["size"] and previous.get("stamp") == f["stamp"]:
                valid = (_digest(out) == previous.get("sha256") if previous.get("method") == "copied"
                         else os.path.samefile(f["source"], out))
            if valid:
                record = previous
            else:
                tmp = out.with_name(out.name + ".partial")
                # Only our unpublished temporary file is removed, never a source.
                tmp.unlink(missing_ok=True)
                method, digest = "hardlinked", ""
                cached = old_files.get(f["source"], {})
                # Reuse unchanged local files across generations. Linking our
                # own previous copy avoids copying an 8 GB base on a tiny mod
                # update, even when Reborn is on another volume.
                reused = False
                if cached.get("stamp") == f["stamp"]:
                    local = _local_path(root, cached["local"])
                    digest = cached["sha256"]
                    checked = (_digest(local) == digest if cached["method"] == "copied"
                               else os.path.samefile(f["source"], local))
                    expected = plan.get("forced_sha256", {}).get(f["source"])
                    if checked and (not expected or _digest(local) == expected):
                        try:
                            os.link(local, tmp)
                            method, reused = cached["method"], True
                        except OSError:
                            pass  # filesystem without hardlinks: verified copy below
                if not reused:
                    method, digest = "hardlinked", ""
                    try:
                        os.link(f["source"], tmp)
                    except OSError:
                        method = "copied"
                        _check_space(dest, max(f["size"], remaining) + SPACE_RESERVE)
                        try:
                            shutil.copyfile(f["source"], tmp)
                            digest = _digest(Path(f["source"]))
                            if tmp.stat().st_size != f["size"] or _digest(tmp) != digest:
                                raise ImportError("Texture copy verification failed; retry Re-import from Reborn.")
                        except BaseException:
                            tmp.unlink(missing_ok=True)
                            raise
                if _stamp(f["source"]) != f["stamp"]:
                    tmp.unlink(missing_ok=True)
                    raise ImportError("Reborn changed during import. Close Reborn and retry; completed files are kept.")
                os.replace(tmp, out)
                record = {**f, "method": method, "sha256": digest}
                log.write(json.dumps(record) + "\n")
                log.flush()
                os.fsync(log.fileno())
            records.append({**record, "local": f"sets/{generation}/{f['relative']}"})
            done += f["size"]
            if (old_files.get(f["source"], {}).get("stamp") != f["stamp"]
                    and completed.get(f["relative"], {}).get("stamp") != f["stamp"]):
                remaining -= f["size"]
            report(done, total)
    if any(_stamp(p) != value for p, value in plan["probes"].items()):
        raise ImportError("Reborn changed during import. Close Reborn and retry; completed files are kept.")
    for layer in plan["layers"]:
        layer["local"] = f"sets/{generation}/{layer['name']}"
    m = {**plan, "schema": STORE_SCHEMA, "files": records, "bytes": total,
         "imported": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    _atomic_json(root / "manifest.json", m)
    return m


def store_summary(m: dict) -> str:
    """One local log line per launcher start; no asset contents or network path."""
    methods = "|".join(sorted({f["method"] for f in m["files"]}))
    return (f"textures source=local-store (imported {m['imported']} from Reborn {m['version']}, "
            f"{len(m['files'])} files, {m['bytes'] / (1 << 20):.1f} MB, {methods})")


def compose(folder: str, icons_setting: str | None, pad_kind: str | None) -> TextureSet:
    """Compose exclusively from our published store. Never inspect Reborn."""
    ts = TextureSet()
    ts.icons, ts.icons_why = resolve_icons(icons_setting, pad_kind)
    m = read_store(folder)
    if not m:
        ts.warnings.append('Local HD textures are missing ("MISSING TEXTURES" art). '
                           'Settings → Advanced → Re-import from Reborn.')
        return ts
    root = Path(folder) / STORE_REL
    ts.warnings.extend(m.get("warnings", []))
    want = PROMPT_PACKAGES.get(ts.icons)
    for layer in m["layers"]:
        path = str(_local_path(root, layer["local"]))
        if layer["kind"] == "base":
            ts.base, ts.base_kind = path, "local-store"
        elif layer["kind"] == "mod":
            ts.packages.append(Package(layer["id"], layer["version"], path, "local-store"))
        elif layer["kind"] == "prompt" and layer["id"] == want:
            ts.prompt = Package(layer["id"], layer["version"], path, "local-store")
    if want and not ts.prompt:
        ts.warnings.append(f"{ICON_LABELS[ts.icons]} button icons were not imported; showing GameCube icons.")
        ts.icons, ts.icons_why = "gamecube", ts.icons_why + ":missing"
    return ts
