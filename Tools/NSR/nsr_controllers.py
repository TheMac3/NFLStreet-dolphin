#!/usr/bin/env python3
"""nsr_controllers.py: use the player's NFL Street Reborn controller setup in NFLStreetRollback.

Reborn ships its own Dolphin ("NFL Street Reborn Dolphin.exe" in <install>/runtime)
and its launcher's Controllers page writes the player's real mapping into that
Dolphin's user folder: Config/GCPadNew.ini, plus controller-related Dolphin.ini
settings ([SDL_Hints], [Input] BackgroundInput). Reborn also bundles ready-made
GCPad profiles (PS4, Xbox One, Switch Pro, keyboards) in runtime/Sys/Profiles/GCPad.

Our netplay Dolphin is a separate portable install (User/ next to Dolphin.exe), so
without this module a friend has to map their controller a second time, and a
PlayStation pad that works in Reborn could come up unmapped or with the wrong
axes. This module:

  * finds Reborn's Dolphin user folder (find_reborn_source), read-only: nothing
    under the Reborn install or Reborn's data folders is ever written;
  * copies the mapping into our User/Config/GCPadNew.ini, making sure the
    player's controller sits in [GCPad1] (the netplay fork always reads the
    local controller from port 1, NSRNetplay.cpp "Our controller is always
    local port 0");
  * copies Reborn's GCPad profiles into User/Config/Profiles/GCPad so they show
    up in our Dolphin's controller window;
  * carries the port device choice (SIDevice0-3: standard controller 6, or
    12 = GameCube controllers on a Wii U / Mayflash USB adapter) into our
    Dolphin.ini, and controller_kind() turns it plus the [GCPad1] mapping into
    the player's controller kind (gc-adapter, xbox, playstation, keyboard,
    ...) for the launcher; see controller_kind for why an adapter player still
    boots a standard controller;
  * carries [SDL_Hints] and the input-related Dolphin.ini keys into our
    Dolphin.ini. Upstream Dolphin (our fork) hard-codes its SDL hints and has
    no [SDL_Hints] section, but SDL3 lets an environment variable override a
    normal-priority hint (SDL_hints.c SDL_SetHintWithPriority), so
    sdl_hint_env() turns the stored section into environment variables for
    every Dolphin launch. Same hints = same controller enumeration and names
    as in Reborn (e.g. SDL_JOYSTICK_DIRECTINPUT=1), so device strings in the
    imported mapping resolve.

Don't-overwrite rule (sync_controllers): the launcher remembers the hash of the
GCPadNew.ini it last wrote. If the file on disk no longer matches, the player
changed the mapping in our Dolphin ("Set up controllers"), and automatic syncs
leave it alone; only the explicit "Use my Reborn controller settings" action
(force=True) replaces it. The very first sync (no record yet) imports even over
an existing file, because the first real two-PC session showed the old
hand-made mappings are exactly what's broken; that file is backed up first.

Deferred: Reborn's Hotkeys.ini is not imported (hotkeys like save states would
desync netplay). Revisit if friends ask for Reborn's hotkeys offline.
"""
from __future__ import annotations

import glob
import hashlib
import json
import os
import re
import shutil
from dataclasses import dataclass, field

GCPAD_INI = "GCPadNew.ini"
DOLPHIN_INI = "Dolphin.ini"
PROFILE_SUBDIR = os.path.join("Profiles", "GCPad")
BACKUP_SUFFIX = ".before-reborn-import"
PORTS = 4

# Dolphin.ini keys that only affect input, copied from Reborn's Dolphin.ini.
# [SDL_Hints] is copied whole (only SDL_* names; see sdl_hint_env). SIDevice0-3
# record which port uses the USB adapter; game launches still pass every
# port's device explicitly (nsr_launcher.dolphin_command), so the stored value
# only feeds controller_kind and our Dolphin's controller window.
INPUT_KEYS = [("Input", "BackgroundInput")] + [
    ("Core", f"{name}{port}") for port in range(PORTS)
    for name in ("SIDevice", "AdapterRumble", "SimulateKonga")]
SI_STANDARD = 6       # SerialInterface::SIDEVICE_GC_CONTROLLER
SI_WIIU_ADAPTER = 12  # SerialInterface::SIDEVICE_WIIU_ADAPTER
SDL_SECTION = "SDL_Hints"
_SDL_NAME = re.compile(r"^SDL_[A-Z0-9_]{1,64}$")


# --- INI (Dolphin's format: "[Section]" then "Key = Value", keys case-insensitive) ---
def parse_ini(text: str) -> list[list]:
    """Ordered [[section, [[key, value], ...]], ...]. Lines before any section
    and comment/blank lines are dropped (Dolphin doesn't write any)."""
    sections: list[list] = []
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith((";", "#", "//")):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = [line[1:-1].strip(), []]
            sections.append(current)
        elif current is not None and "=" in line:
            key, _, value = line.partition("=")
            current[1].append([key.strip(), value.strip()])
    return sections


def dump_ini(sections: list[list]) -> str:
    out = []
    for name, items in sections:
        out.append(f"[{name}]")
        out += [f"{k} = {v}" for k, v in items]
    return "\n".join(out) + "\n"


def get_section(sections: list[list], name: str) -> list | None:
    for sec in sections:
        if sec[0].lower() == name.lower():
            return sec[1]
    return None


def get_value(items: list | None, key: str) -> str | None:
    for k, v in items or ():
        if k.lower() == key.lower():
            return v
    return None


def set_value(sections: list[list], section: str, key: str, value: str) -> None:
    items = get_section(sections, section)
    if items is None:
        items = []
        sections.append([section, items])
    for pair in items:
        if pair[0].lower() == key.lower():
            pair[1] = value
            return
    items.append([key, value])


def read_text(path: str) -> str | None:
    try:
        with open(path, encoding="utf-8-sig", errors="surrogateescape") as f:
            return f.read()
    except OSError:
        return None


def write_text(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)


def text_hash(text: str | None) -> str | None:
    if text is None:
        return None
    return hashlib.sha256(text.encode("utf-8", "surrogateescape")).hexdigest()[:16]


# --- finding Reborn's Dolphin user folder ---------------------------------------
@dataclass
class RebornSource:
    user_dir: str               # Reborn Dolphin's user folder (has Config/GCPadNew.ini)
    gcpad: str                  # .../Config/GCPadNew.ini
    dolphin_ini: str | None     # .../Config/Dolphin.ini, if present
    profile_dirs: list[str] = field(default_factory=list)  # GCPad profile folders to copy


def data_roots(install: str | None, env=os.environ) -> list[str]:
    """Folders where Reborn's launcher may keep per-user data. reborn-launcher.exe
    (3.0, Tauri app id com.nflstreetreborn.launcher) names a data root with
    config/settings.json and dolphin-users/, plus a legacy %APPDATA%\\NFL Street
    Reborn (Mason reports game stats under %APPDATA%\\nflstreetreborn); we
    check every plausible spelling and keep the ones that exist."""
    roots = [install] if install else []
    for var in ("LOCALAPPDATA", "APPDATA"):
        base = env.get(var)
        if base:
            roots += [os.path.join(base, name) for name in
                      ("NFL Street Reborn", "com.nflstreetreborn.launcher", "nfl-street-reborn",
                       "nflstreetreborn")]
    return [r for r in dict.fromkeys(roots) if r and os.path.isdir(r)]


def _settings_user_dirs(root: str) -> list[str]:
    """user_dir values in Reborn's settings.json files (any nesting depth)."""
    found = []

    def walk(node):
        if isinstance(node, dict):
            for k, v in node.items():
                if k == "user_dir" and isinstance(v, str) and v:
                    found.append(v if os.path.isabs(v) else os.path.join(root, v))
                else:
                    walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    for rel in (("config", "settings.json"), ("settings.json",)):
        try:
            with open(os.path.join(root, *rel), encoding="utf-8") as f:
                walk(json.load(f))
        except (OSError, ValueError):
            pass
    return found


def candidate_user_dirs(install: str | None, env=os.environ) -> list[str]:
    """Every folder that might be Reborn Dolphin's user folder, best guesses first."""
    roots = data_roots(install, env)
    cands: list[str] = []
    for root in roots:
        cands += _settings_user_dirs(root)
    if install:
        cands += [os.path.join(install, "runtime", "User"), os.path.join(install, "User")]
    for root in roots:
        cands += sorted(glob.glob(os.path.join(root, "dolphin-users", "*")))
    if env.get("APPDATA"):  # Reborn 2.x kept a non-portable Dolphin user folder here
        cands.append(os.path.join(env["APPDATA"], "NFL Street Reborn", "Dolphin Emulator"))
    return list(dict.fromkeys(os.path.normpath(c) for c in cands))


GAME_ID = "GN7E69"


def reborn_load_path(install: str | None, env=os.environ) -> str | None:
    """Dolphin LoadPath with the textures Reborn's own Dolphin really shows.

    Reborn's launcher composes its texture library plus the player's enabled
    mods (button-prompt packs, footballs, ...) into the user folder's
    Load/Textures/<gameid> (e.g. %APPDATA%\\NFL Street Reborn\\dolphin-users\\
    fresh\\Load). Prefer the newest composed tree; fall back to the raw
    library. Since 2026-10-23 the launcher composes its own set by reference
    (nsr_textures) and uses this only as a last-resort base: the composed tree
    carries Reborn's own button-icon choice. (The "MISSING TEXTURES" art seen
    in netplay came from HD textures never being enabled, -C GFX.* instead of
    -C Graphics.*, not from the raw library.) Read only.
    """
    best = None
    for i, user_dir in enumerate(candidate_user_dirs(install, env)):
        tex = os.path.join(user_dir, "Load", "Textures", GAME_ID)
        try:
            if not os.path.isdir(tex) or not os.listdir(tex):
                continue
            mtime = os.path.getmtime(tex)
        except OSError:
            continue
        key = (mtime, -i)
        if best is None or key > best[0]:
            best = (key, os.path.join(user_dir, "Load"))
    if best:
        return best[1]
    # No composed tree yet (Reborn never launched on this PC): Reborn's
    # downloaded texture library, laid out as <root>/library/textures/<gameid>,
    # still beats the installer's seed copy in content/textures.
    for root in data_roots(install, env):
        lib = os.path.join(root, "library")
        if os.path.isdir(os.path.join(lib, "textures", GAME_ID)):
            return lib
    return os.path.join(install, "content") if install else None


def find_reborn_source(install: str | None, env=os.environ) -> RebornSource | None:
    """The Reborn Dolphin user folder holding the player's mapping. Several can
    exist (the install's stock runtime/User and launcher-made copies); the
    player's real one is the GCPadNew.ini the Controllers page wrote last, so
    the newest file wins (ties: discovery order)."""
    best = None
    for i, user_dir in enumerate(candidate_user_dirs(install, env)):
        gcpad = os.path.join(user_dir, "Config", GCPAD_INI)
        try:
            mtime = os.path.getmtime(gcpad)
        except OSError:
            continue
        if best is None or mtime > best[0]:
            best = (mtime, i, user_dir, gcpad)
    if best is None:
        return None
    _, _, user_dir, gcpad = best
    dini = os.path.join(user_dir, "Config", DOLPHIN_INI)
    sys_dirs = [os.path.join(os.path.dirname(user_dir), "Sys")]
    if install:
        sys_dirs += [os.path.join(install, "runtime", "Sys"), os.path.join(install, "Sys")]
    profile_dirs = [os.path.join(d, PROFILE_SUBDIR) for d in dict.fromkeys(sys_dirs)]
    profile_dirs.append(os.path.join(user_dir, "Config", PROFILE_SUBDIR))  # player-saved
    return RebornSource(user_dir, gcpad, dini if os.path.isfile(dini) else None,
                        [d for d in profile_dirs if os.path.isdir(d)])


# --- building our mapping ----------------------------------------------------------
def _is_mapped(items: list | None) -> bool:
    """A port the player actually set up: a device plus at least one binding."""
    if not items or not get_value(items, "Device"):
        return False
    keys = {k.lower() for k, _ in BINDINGS}
    return any(k.lower() in keys and v for k, v in items)


def port1_mapping(reborn_text: str) -> str:
    """Reborn's GCPadNew.ini with the player's controller in [GCPad1]. Reborn's
    launcher can assign the player to any of Players 1-4; netplay only reads
    port 1, so if GCPad1 isn't set up but another port is, the two swap.
    Other ports are kept for offline practice with several local pads."""
    sections = parse_ini(reborn_text)
    for port in range(1, PORTS + 1):
        if get_section(sections, f"GCPad{port}") is None:
            sections.append([f"GCPad{port}", []])
    first = get_section(sections, "GCPad1")
    if not _is_mapped(first):
        for port in range(2, PORTS + 1):
            other = get_section(sections, f"GCPad{port}")
            if _is_mapped(other):
                for sec in sections:
                    if sec[0].lower() == "gcpad1":
                        sec[1] = other
                    elif sec[0].lower() == f"gcpad{port}":
                        sec[1] = first
                break
    sections.sort(key=lambda s: s[0].lower())
    return dump_ini(sections)


def merge_input_settings(ours_text: str | None, reborn_text: str | None) -> str | None:
    """Our Dolphin.ini with Reborn's input-only keys and [SDL_Hints] carried
    over; everything else in ours untouched. None: nothing to carry."""
    if not reborn_text:
        return None
    theirs = parse_ini(reborn_text)
    ours = parse_ini(ours_text or "")
    changed = False
    for section, key in INPUT_KEYS:
        value = get_value(get_section(theirs, section), key)
        if value is not None:
            set_value(ours, section, key, value)
            changed = True
    hints = [(k, v) for k, v in get_section(theirs, SDL_SECTION) or () if _SDL_NAME.match(k)]
    if hints:
        for sec in ours:
            if sec[0].lower() == SDL_SECTION.lower():
                sec[1] = []  # Reborn's hint set replaces ours as a whole
        for k, v in hints:
            set_value(ours, SDL_SECTION, k, v)
        changed = True
    return dump_ini(ours) if changed else None


def sdl_hint_env(our_user_dir: str) -> dict[str, str]:
    """Environment variables for the SDL hints stored in our Dolphin.ini
    [SDL_Hints] (imported from Reborn). Names are checked to be SDL_* so a
    hand-edited file can't inject other variables."""
    text = read_text(os.path.join(our_user_dir, "Config", DOLPHIN_INI))
    items = get_section(parse_ini(text or ""), SDL_SECTION) or []
    return {k: v[:64] for k, v in items if _SDL_NAME.match(k) and "\0" not in v}


# --- the sync ---------------------------------------------------------------------
@dataclass
class SyncResult:
    action: str        # imported | first-import | updated | up-to-date | kept-yours | no-reborn | error
    record: dict       # new launcher-config record (store under cfg["controllers"])
    message: str = ""
    backup: str | None = None
    profiles: list[str] = field(default_factory=list)  # profile files copied


def _copy_profiles(source: RebornSource, our_user_dir: str, overwrite: bool) -> list[str]:
    dest = os.path.join(our_user_dir, "Config", PROFILE_SUBDIR)
    copied = []
    for d in source.profile_dirs:
        for path in sorted(glob.glob(os.path.join(d, "*.ini"))):
            target = os.path.join(dest, os.path.basename(path))
            if os.path.exists(target) and not overwrite:
                continue
            try:
                os.makedirs(dest, exist_ok=True)
                shutil.copyfile(path, target)
                copied.append(os.path.basename(path))
            except OSError:
                pass
    return copied


def input_digest(dolphin_ini_text: str | None) -> str | None:
    """Hash of the INPUT_KEYS values in a Dolphin.ini (what the sync manages)."""
    sections = parse_ini(dolphin_ini_text or "")
    values = [f"{sec}.{key}={get_value(get_section(sections, sec), key)}" for sec, key in INPUT_KEYS]
    return text_hash("\n".join(values))


def _merge_dolphin_ini(our_user_dir: str, source: RebornSource, record: dict, force: bool) -> None:
    """Carry Reborn's input keys into our Dolphin.ini, under the same
    don't-overwrite rule as the mapping: if the player changed them in our
    Dolphin since the last sync (e.g. set Port 1 to the USB adapter in our
    controller window), keep theirs unless forced. record["input_written"]
    remembers what we wrote."""
    dini_path = os.path.join(our_user_dir, "Config", DOLPHIN_INI)
    ours = read_text(dini_path)
    written = record.get("input_written")
    if not force and written and input_digest(ours) != written:
        return
    merged = merge_input_settings(ours, read_text(source.dolphin_ini) if source.dolphin_ini else None)
    if merged is not None and merged != ours:
        write_text(dini_path, merged)
    record["input_written"] = input_digest(merged if merged is not None else ours)


def sync_controllers(our_user_dir: str, source: RebornSource | None, record: dict | None,
                     force: bool = False) -> SyncResult:
    """Bring Reborn's controller setup into our Dolphin (see module docstring).

    record: what the last sync wrote ({"written": hash, "source": path, ...}),
    from the launcher config. force: the player clicked "Use my Reborn
    controller settings", so replace even a mapping they made in our Dolphin.
    Reborn's files are only read."""
    record = dict(record or {})
    if source is None:
        return SyncResult("no-reborn", record, "Couldn't find NFL Street Reborn's controller settings.")
    reborn_text = read_text(source.gcpad)
    if reborn_text is None:
        return SyncResult("error", record, f"Couldn't read {source.gcpad}.")
    wanted = port1_mapping(reborn_text)
    ours_path = os.path.join(our_user_dir, "Config", GCPAD_INI)
    ours = read_text(ours_path)
    ours_hash, wanted_hash = text_hash(ours), text_hash(wanted)

    if force:
        action = "imported"
    elif not record.get("written"):
        action = "first-import"
    elif ours is not None and ours_hash != record["written"]:
        return SyncResult("kept-yours", record,
                          "Using the mapping you made in NFLStreetRollback's controller setup.")
    elif ours_hash == wanted_hash:
        # Same mapping, but the port device (e.g. switching to the USB adapter,
        # which needs no mapping) lives in Dolphin.ini: keep that in step too.
        try:
            _merge_dolphin_ini(our_user_dir, source, record, force)
        except OSError as e:
            return SyncResult("error", record, f"Couldn't write the controller settings: {e}")
        return SyncResult("up-to-date", record, "Using your Reborn controller settings.")
    else:
        action = "updated"

    backup = None
    try:
        if ours is not None and ours_hash != wanted_hash:
            backup = ours_path + BACKUP_SUFFIX
            write_text(backup, ours)
        write_text(ours_path, wanted)
        _merge_dolphin_ini(our_user_dir, source, record, force)
    except OSError as e:
        return SyncResult("error", record, f"Couldn't write the controller settings: {e}")
    profiles = _copy_profiles(source, our_user_dir, overwrite=force or action == "first-import")
    record.update({"written": wanted_hash, "source": source.gcpad, "source_hash": text_hash(reborn_text)})
    return SyncResult(action, record, "Using your Reborn controller settings.", backup, profiles)


# --- display ----------------------------------------------------------------------
# Labels for the controls page, in GameCube order: (ini key, label).
BINDINGS = [("Buttons/A", "A"), ("Buttons/B", "B"), ("Buttons/X", "X"), ("Buttons/Y", "Y"),
            ("Buttons/Z", "Z"), ("Buttons/Start", "Start"), ("Triggers/L", "L"), ("Triggers/R", "R"),
            ("Main Stick/Up", "Stick up"), ("Main Stick/Down", "Stick down"),
            ("Main Stick/Left", "Stick left"), ("Main Stick/Right", "Stick right"),
            ("C-Stick/Up", "C up"), ("C-Stick/Down", "C down"), ("C-Stick/Left", "C left"),
            ("C-Stick/Right", "C right"), ("D-Pad/Up", "D-pad up"), ("D-Pad/Down", "D-pad down"),
            ("D-Pad/Left", "D-pad left"), ("D-Pad/Right", "D-pad right")]

_FAMILIES = [("xbox", "Xbox controller"), ("xinput", "Xbox controller"),  # before "wireless controller"
             ("wireless controller", "PlayStation 4 controller"), ("ps4", "PlayStation 4 controller"),
             ("dualsense", "PlayStation 5 controller"), ("ps5", "PlayStation 5 controller"),
             ("pro controller", "Switch Pro controller"), ("switch", "Switch Pro controller"),
             ("gamecube", "GameCube controller"), ("keyboard", "Keyboard")]


def describe_device(device: str | None) -> dict:
    """"DInput/0/Wireless Controller" -> backend, name and a friendly family."""
    if not device:
        return {"device": "", "backend": "", "name": "", "family": "No controller"}
    backend, _, rest = device.partition("/")
    _, _, name = rest.partition("/")
    name = name or rest or device
    low = f"{backend} {name}".lower()
    family = next((f for k, f in _FAMILIES if k in low), name)
    return {"device": device, "backend": backend, "name": name, "family": family}


# --- the player's controller kind (what port 1 reads) -----------------------------
# Kinds, by the mapping's device string (backend/index/name as Dolphin writes it):
# first match wins ("Xbox Wireless Controller" is an Xbox pad, plain "Wireless
# Controller" is how DInput names a DualShock 4 / DualSense).
_KINDS = [("xbox", "xbox"), ("xinput", "xbox"), ("dualsense", "playstation"),
          ("ps5", "playstation"), ("dualshock", "playstation"), ("ps4", "playstation"),
          ("ps3", "playstation"), ("playstation", "playstation"),
          ("wireless controller", "playstation"), ("pro controller", "switch"),
          ("switch", "switch"), ("keyboard", "keyboard"), ("gamecube", "gamecube-pc")]


@dataclass
class PadKind:
    kind: str                  # gc-adapter | xbox | playstation | switch | keyboard | gamecube-pc | gamepad | none
    device: str                # [GCPad1] Device, or "GCAdapter/<port>" for the adapter
    adapter_channel: int | None = None  # adapter port 0-3 (gc-adapter only)

    def env(self) -> dict[str, str]:
        """Environment for the fork: NSR_PAD_ADAPTER turns on its adapter
        source; NSR_PAD_KIND/NSR_PAD_DEVICE are only logged (nsr-p*.log)."""
        env = {"NSR_PAD_KIND": self.kind,
               "NSR_PAD_DEVICE": "".join(c for c in self.device if c.isprintable())[:96]}
        if self.adapter_channel is not None:
            env["NSR_PAD_ADAPTER"] = str(self.adapter_channel)
        return env


def kind_of_device(device: str | None) -> str:
    if not device:
        return "none"
    backend, _, rest = device.partition("/")
    low = f"{backend} {rest}".lower()
    return next((k for key, k in _KINDS if key in low), "gamepad")


def _si_device(core: list | None, port: int) -> int | None:
    try:
        return int(get_value(core, f"SIDevice{port}") or "")
    except ValueError:
        return None


def controller_kind(our_user_dir: str) -> PadKind:
    """The player's controller for port 1, from our (imported) Dolphin.ini and
    GCPadNew.ini, in Reborn's precedence: port 1's adapter, else port 1's
    mapping, else the first other port set to the adapter.

    An adapter player is still booted as a standard controller (SIDevice 6):
    the stock adapter device (12) is visible to the game (it answers "no
    controller" while the adapter port is empty and makes the game re-read the
    pad's resting position into its own memory on one PC only), so netplay
    peers would no longer run identical games. The fork instead reads the
    adapter host-side as port 1's input (NSR_PAD_ADAPTER; fork
    Core/NSR/NSRPadSource.h), the same way it reads a mapped Xbox pad."""
    config = os.path.join(our_user_dir, "Config")
    core = get_section(parse_ini(read_text(os.path.join(config, DOLPHIN_INI)) or ""), "Core")
    pads = parse_ini(read_text(os.path.join(config, GCPAD_INI)) or "")
    first = get_section(pads, "GCPad1")
    device = get_value(first, "Device") or ""
    if _si_device(core, 0) == SI_WIIU_ADAPTER:
        return PadKind("gc-adapter", "GCAdapter/0", 0)
    if _is_mapped(first):
        return PadKind(kind_of_device(device), device)
    for port in range(1, PORTS):
        if _si_device(core, port) == SI_WIIU_ADAPTER:
            return PadKind("gc-adapter", f"GCAdapter/{port}", port)
    return PadKind(kind_of_device(device) if device else "none", device)


def port1_summary(our_user_dir: str) -> dict:
    """What our Dolphin will use for port 1: device and the main bindings."""
    items = get_section(parse_ini(read_text(os.path.join(our_user_dir, "Config", GCPAD_INI)) or ""),
                        "GCPad1")
    info = describe_device(get_value(items, "Device"))
    info["bindings"] = [{"control": label, "input": (get_value(items, key) or "").replace("`", "")}
                        for key, label in BINDINGS]
    info["mapped"] = _is_mapped(items)
    pad = controller_kind(our_user_dir)
    info["kind"] = pad.kind
    if pad.adapter_channel is not None:  # the adapter needs no mapping
        info.update({"device": pad.device, "backend": "GCAdapter", "mapped": True,
                     "name": f"USB adapter port {pad.adapter_channel + 1}",
                     "family": f"GameCube controller (USB adapter port {pad.adapter_channel + 1})"})
    return info
