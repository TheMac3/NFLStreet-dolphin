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

What we do: compose the same set *by reference*. compose() returns ordered
folders, lowest precedence first: the pristine library, the packages the player
turned on in Reborn (prompt packages excluded), then the prompt package for the
player's button-icon choice. write_list() records them in
<our User>/Load/NSRTextures/<game id>.txt, which the fork's
VideoCommon/HiresTextures.cpp reads as an ordered overlay (later folders win).
Nothing is linked, copied or written under Reborn's folders, it costs no disk
space, and it does not depend on Reborn having been launched recently (its
composed tree would also carry Reborn's own prompt choice, which we want to
choose per controller). Hardlinking our own composed tree was the alternative:
rejected because it needs the same volume, a copy fallback would duplicate GBs,
and a stale tree would need invalidation.

Button icons are per-PC presentation: textures never reach guest RAM in NSR
netplay (the fork answers CPU EFB peeks canonically and keeps EFB/XFB copies out
of RAM), so each peer may show its own set.

Deferred: a package that must *remove* a library texture (not replace it) is not
expressible (the fork's overlay has no masking); none of Reborn's packages do.
Revisit if Reborn ships one. Soundtrack packages are ignored (not textures).
"""
from __future__ import annotations

import json
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


def compose(install: str | None, icons_setting: str | None, pad_kind: str | None,
            env=os.environ) -> TextureSet:
    """The player's texture set (see the module docstring). Never raises."""
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
