#!/usr/bin/env python3
"""nsr_launcher.py: "NFLStreetRollback", the friend-facing launcher for NFLStreet netplay
("NFLStreetRollback.exe"; short form "NSRB"; the module, NSR_* variables and
nsr-*.json/.txt file names keep their old internal names on purpose).

A window that replaces command lines and .bat files. It lives in the
NFLStreet-dolphin folder (next to Dolphin.exe) and does four things:

  1. Game: find NFL Street Reborn's NSR.iso (or browse to it), build the
     rollback netplay image (nsr_patcher.prepare_image), and show its
     fingerprint. An image from an older (lockstep) download is detected and
     must be rebuilt.
  2. Play: Host (Create room: shows a code like NSRB-4K7QX, lists who joined,
     Start) or Join (type the code, wait for Start), via the rendezvous server
     (room codes + hole punching, so usually no router setup). "Host by IP
     address" / typing an IP is the advanced fallback. Then it starts the
     netplay Dolphin with every setting filled in: rollback netplay (stage 4:
     menus in confirmed lockstep, matches in rollback, see ROLLBACK_ENV), one
     input delay chosen by the host for everyone, and passive lag/rollback
     telemetry unless the player unticks it (then NSR_TELEMETRY=0).
  3. Practice offline, and "Set up controllers" (opens Dolphin's own window).
  4. Controllers: the player's NFL Street Reborn controller setup is copied
     into this Dolphin automatically (nsr_controllers.py), so a pad mapped in
     Reborn's launcher works here without mapping it twice.
  5. Game results: new per-game stats files that Reborn's own Dolphin writes
     (GameStats/, stats/queue/) are uploaded in the background to Mason's
     server for the shared Stats page (nsr_gamestats.py, docs/stats.md),
     unless the player unticks "Upload game results".

Reborn is never modified: its files are only read, this Dolphin runs in
portable mode with its own settings (User/ next to it). Offline saves persist;
netplay uses fresh disposable cards and canonical SRAM owned by the fork, and
Reborn's HD textures are loaded read-only from its install.

Structure: pure helpers (dolphin_command & co., unit-tested), then Launcher,
the UI-agnostic state machine every button calls (also tested, with fake
lobby/Popen), then two views over it: the web UI (nsr_launcher_ui.html in
pywebview / Edge WebView2, styled as a sibling of Reborn's launcher) and the
plain tkinter fallback for PCs without WebView2 (choose_ui).

Why pywebview over customtkinter: the Reborn launcher look (cards, gradients,
big type, sidebar) is plain CSS, while customtkinter would still be
hand-placed Tk widgets; WebView2 ships with Windows 11 and current Windows 10,
and the tkinter fallback covers the rest. Cost: ~10 MB more exe (pythonnet).

Run from source:  python3 tools/nsr_launcher.py   (pywebview if installed, else tkinter)
Dev overrides:    NSR_APP_DIR (folder with Dolphin + nsr-net.iso),
                  NSR_DOLPHIN (Dolphin executable), NSR_DOLPHIN_ARGS (extra args),
                  NSR_UI=tk|web (force a view), NSR_UI_DEBUG=1 (WebView2 devtools)
Self-test (CI):   nsr_launcher.py --selftest [--tk]  builds the UI, waits until the
                  page has rendered from a real snapshot, closes; exit 0 = ok.
"""
from __future__ import annotations

import json
import os
import secrets
import shlex
import socket
import subprocess
import sys
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import isopatch  # noqa: E402
import nsr_controllers as controllers  # noqa: E402
import nsr_gamestats as gamestats  # noqa: E402
import nsr_lobby as lobby  # noqa: E402
import nsr_patcher as patcher  # noqa: E402

APP_TITLE = "NFLStreetRollback"  # window title; user-facing product name
CONFIG_NAME = "nsr-launcher.json"
# Rollback input delay in frames, the same for every player (the host's
# choice is sent to all). 1 is the tested release value (release plan 2.3:
# "playtest fixed setting" until automatic delay exists); 0..MAX_DELAY offered.
DEFAULT_DELAY = 1
MAX_DELAY = 4
# Lockstep input delay for the menus around rollback matches (stage 4), the
# fork's default and what every stage-4 gate ran with. Menus wait on real
# input each frame, so a larger value only smooths them; the host's choice
# above is the rollback (match) delay, NSR_RB_DELAY.
MENU_LOCKSTEP_DELAY = 2

# Stage-4 rollback profile (release plan 3.2), exactly what the rollback
# netplay gates ran with (tools/detrun.py "rollback" profile) minus the fixed
# NSR_RB_VTIME_FROM epoch: without it the fork agrees match entry/exit on the
# confirmed lockstep menu timeline by itself. NSR_RB_DELAY is added per match.
# NSR_RB_IO_IDLE/AUDIO_ROLLBACK are implied by NSR_RB_NET in the current fork
# and set anyway so the telemetry config record states them; INSTANT_DISC
# (zero-latency DVD inside rollback sessions) matches every tested run.
ROLLBACK_ENV = {
    "NSR_RB_NET": "1",
    "NSR_RB_MAX_PRED": "7",
    "NSR_RB_TIME_SYNC": "1",
    "NSR_RB_INSTANT_DISC": "1",
    "NSR_RB_IO_IDLE": "1",
    "NSR_RB_AUDIO_ROLLBACK": "1",
}

# Audio for netplay launches (host-side only: the mixer and the throttle's
# catch-up allowance, never emulated state, so no determinism impact).
# Rollback resimulation and time-sync slowdowns stall audio production for a
# few frames; with Dolphin's default 80 ms buffer that is heard as chop. This
# Dolphin (2512+) has no AudioStretch/AudioStretchMaxLatency any more: the
# granule mixer (AudioCommon/Mixer.cpp) stretches by replaying recent audio
# when the queue runs dry ("Fill Audio Gaps", Core.AudioFillGaps), sized by
# Core.AudioBufferSize (16..512 ms, steps of 8). 128 ms (default 80) gives
# the mixer room to absorb those stalls for ~48 ms more audio latency, which
# is not felt in a football game. Offline practice keeps Dolphin's defaults.
NETPLAY_AUDIO_BUFFER_MS = 128
NETPLAY_INPUT_ARGS = ["-C", "Dolphin.Input.BackgroundInput=True"]
NETPLAY_AUDIO_ARGS = ["-C", "Dolphin.Core.AudioFillGaps=True",
                      "-C", f"Dolphin.Core.AudioBufferSize={NETPLAY_AUDIO_BUFFER_MS}"]


# --- pure helpers (tested without a GUI) --------------------------------------
def app_dir() -> str:
    if os.environ.get("NSR_APP_DIR"):
        return os.environ["NSR_APP_DIR"]
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.getcwd()


GAME_STATS_NOTE = ("Final scores and box-score stats of the games you finish in NFL Street Reborn (the "
                   "files its Dolphin saves after the Game Stats/MVP screens), your name above and a random "
                   "ID for this install. Never other files, folder paths, or your PC's name. Untick to upload "
                   "nothing.")
TELEMETRY_FILE = "nsr-telemetry.txt"  # shipped next to the launcher: url=..., key=... (POC)


def telemetry_config(folder: str) -> dict[str, str]:
    """Fallback ingest settings for rooms whose server grants no token (direct IP,
    older room servers): NSR_TELEMETRY_URL/NSR_TELEMETRY_KEY, else nsr-telemetry.txt
    ("url=https://..." and "key=<POC shared key>" lines). Empty: no fallback."""
    cfg = {"url": os.environ.get("NSR_TELEMETRY_URL", ""), "key": os.environ.get("NSR_TELEMETRY_KEY", "")}
    try:
        with open(os.path.join(folder, TELEMETRY_FILE)) as f:
            for line in f:
                k, _, v = line.strip().partition("=")
                if k in cfg and not cfg[k]:
                    cfg[k] = v.strip()
    except OSError:
        pass
    return cfg


def telemetry_env(folder: str, start: lobby.StartInfo, name: str, fingerprint: str,
                  enabled: bool = True) -> dict[str, str]:
    """Environment for the fork's passive telemetry (NSRTelemetry.h, docs/telemetry.md).
    The room server's per-session token wins; the shared POC key is the fallback."""
    if not enabled:
        return {"NSR_TELEMETRY": "0"}
    env = {"NSR_PLAYER_NAME": name[:24], "NSR_FINGERPRINT": fingerprint or "",
           "NSR_ROOM_CODE": start.room_code,
           # Same id on every machine in the match: the room id, else the agreed seed.
           "NSR_TELEMETRY_SESSION": start.room or f"seed-{start.seed:08x}"}
    fallback = telemetry_config(folder)
    if start.telemetry_url and start.telemetry_token:
        env.update({"NSR_TELEMETRY_URL": start.telemetry_url,
                    "NSR_TELEMETRY_TOKEN": start.telemetry_token})
    elif fallback["url"] and fallback["key"]:
        env.update({"NSR_TELEMETRY_URL": fallback["url"], "NSR_TELEMETRY_TOKEN": fallback["key"]})
    else:
        env["NSR_TELEMETRY_LOCAL"] = "1"  # nothing to send to: keep a local file to share
    return env


def dolphin_command(folder: str, start: lobby.StartInfo | None, reborn_install: str | None,
                    open_ui: bool = False, name: str = "Player", fingerprint: str = "",
                    telemetry: bool = True) -> tuple[list[str], dict[str, str]]:
    """Command line and environment for the netplay Dolphin.

    start=None: practice offline. open_ui=True: just open Dolphin's window
    (controller setup), no game. The fork installs its canonical Netplay config
    layer and disposable empty cards for NSR_NETPLAY, then agrees the effective
    settings with every peer before guest boot. These flags also document the
    launcher profile; local UI/game config cannot override the fork layer.
    """
    exe = os.environ.get("NSR_DOLPHIN") or os.path.join(folder, "Dolphin.exe")
    args = [exe, "-u", os.path.join(folder, "User")]
    args += shlex.split(os.environ.get("NSR_DOLPHIN_ARGS", ""))
    env = {k: v for k, v in os.environ.items() if not k.startswith("NSR_")}
    # Reborn's SDL hints (imported into our Dolphin.ini [SDL_Hints]) for every
    # launch, controller window included, so devices enumerate and are named
    # exactly as in Reborn and the imported mapping's Device lines resolve.
    env.update(controllers.sdl_hint_env(os.path.join(folder, "User")))
    if open_ui:
        return args, env
    players = start.players if start else 1
    load_path = controllers.reborn_load_path(reborn_install, os.environ) if reborn_install else None
    if load_path:
        # Reborn's composed texture tree (library + the player's mods), as its
        # own Dolphin loads it; see controllers.reborn_load_path. Cached like
        # Reborn's GFX.ini so textures don't stream in mid-match.
        args += ["-C", f"Dolphin.General.LoadPath={load_path}",
                 "-C", "GFX.Settings.HiresTextures=True",
                 "-C", "GFX.Settings.CacheHiresTextures=True"]
    # Some Quick Play paths make Reborn's own code read through a null pointer
    # (harmless under Dolphin, which returns 0); keep those warnings out of
    # popup dialogs during a match. They still go to Dolphin's log.
    args += ["-C", "Dolphin.Interface.UsePanicHandlers=False"]
    args += ["-C", "Dolphin.Core.CPUThread=False",
             "-C", "Dolphin.Core.EnableCustomRTC=True",
             "-C", "Dolphin.Core.CustomRTCValue=0x30000000"]
    for port in range(4):  # controllers in ports 1..players; the rest empty
        args += ["-C", f"Dolphin.Core.SIDevice{port}={6 if port < players else 0}"]
    # The player's controller (nsr_controllers.controller_kind), netplay and
    # offline alike. Port 1 stays a standard controller (6) even for a USB
    # GameCube adapter: NSR_PAD_ADAPTER makes the fork read the adapter as
    # port 1's input instead of switching the port to the adapter device (12),
    # which the game can tell apart (see controller_kind). NSR_PAD_KIND/DEVICE
    # are only logged, so "my controller isn't recognized" is diagnosable.
    env.update(controllers.controller_kind(os.path.join(folder, "User")).env())
    # Per-match RNG seed: agreed in the lobby for netplay; fresh per launch
    # offline, so Quick Play differs every time either way.
    env["NSR_SEED"] = str(start.seed if start else secrets.randbits(32))
    if start:
        env.update({
            "NSR_NETPLAY": "1",
            # Compatibility is independent of the optional telemetry upload.
            "NSR_FINGERPRINT": fingerprint or "",
            "NSR_PLAYER": str(start.player),
            "NSR_PLAYERS": str(start.players),
            "NSR_PORT": str(lobby.PORT),
            "NSR_TOPOLOGY": "star",
            "NSR_DELAY": str(start.delay),
            "NSR_LOG": os.path.join(folder, f"nsr-p{start.player}.log"),
        })
        if start.mode == "rollback":
            # Stage 4 keeps menus in lockstep and agrees match boundaries on
            # the frame-keyed channel, so an operator-supplied epoch is both
            # unnecessary and unsafe here.
            env.update(ROLLBACK_ENV)
            env["NSR_RB_DELAY"] = str(start.delay)
            env["NSR_DELAY"] = str(MENU_LOCKSTEP_DELAY)
            args += ["-C", "Dolphin.Core.FastDiscSpeed=True"]
        args += NETPLAY_AUDIO_ARGS
        # A joiner's Dolphin is started by a network message, not a click, so
        # Windows' foreground lock keeps its window behind the launcher; with
        # Dolphin's default (BackgroundInput off) that player's controller is
        # then ignored. Host-side input policy only: pads are still exchanged.
        args += NETPLAY_INPUT_ARGS
        if start.rendezvous:
            # Room code: hole punching through the rendezvous server.
            env.update({"NSR_RENDEZVOUS": start.rendezvous, "NSR_ROOM": start.room,
                        "NSR_KEY": start.key, "NSR_LAN_IP": lan_address()})
            env.pop("NSR_PORT", None)
        elif start.player > 0:
            env["NSR_PEERS"] = f"{start.host_address}:{lobby.PORT}"
        env.update(telemetry_env(folder, start, name, fingerprint, telemetry))
    if not os.environ.get("NSR_DOLPHIN"):
        args.append("-b")  # Dolphin closes when the game stops (Qt build)
    args += ["-e", os.path.join(folder, patcher.OUTPUT_NAME)]
    return args, env


RENDEZVOUS_FILE = "nsr-rendezvous.txt"


def rendezvous_server(folder: str) -> str | None:
    """Room-server address ("host:port"): NSR_RENDEZVOUS_SERVER, else the
    nsr-rendezvous.txt shipped next to the launcher. None: room codes off."""
    if os.environ.get("NSR_RENDEZVOUS_SERVER"):
        return os.environ["NSR_RENDEZVOUS_SERVER"]
    try:
        with open(os.path.join(folder, RENDEZVOUS_FILE)) as f:
            value = f.read().strip()
            return value or None
    except OSError:
        return None


def looks_like_ip(text: str) -> bool:
    """Join box: an IP/hostname with dots means direct mode; else a room code."""
    return "." in text or text.count(":") > 1


def lan_address() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))  # no packet is sent; picks the outgoing interface
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def public_address(timeout: float = 4.0) -> str | None:
    try:
        with urllib.request.urlopen("https://api.ipify.org", timeout=timeout) as r:
            return r.read().decode().strip()
    except Exception:
        return None


def load_config(folder: str) -> dict:
    try:
        with open(os.path.join(folder, CONFIG_NAME)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_config(folder: str, cfg: dict) -> None:
    try:
        with open(os.path.join(folder, CONFIG_NAME), "w") as f:
            json.dump(cfg, f, indent=1)
    except OSError:
        pass




LOG_NAME = "nsr-launcher.log"


def log(folder: str, msg: str) -> None:
    """Append to nsr-launcher.log (the windowed exe has no console). Kept small."""
    path = os.path.join(folder, LOG_NAME)
    try:
        if os.path.getsize(path) > 256 * 1024:
            os.replace(path, path + ".old")
    except OSError:
        pass
    try:
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}\n")
    except OSError:
        pass


def resource_path(name: str) -> str:
    """A data file bundled next to this module (or inside the PyInstaller exe)."""
    base = getattr(sys, "_MEIPASS", None) or os.path.dirname(os.path.abspath(__file__))
    return os.path.join(base, name)


UI_HTML = "nsr_launcher_ui.html"
# Evergreen WebView2 Runtime client id (Microsoft's documented detection keys).
WEBVIEW2_CLIENT = "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"


def _read_registry(hive: str, key: str, value: str) -> str | None:
    try:
        import winreg
        root = {"HKLM": winreg.HKEY_LOCAL_MACHINE, "HKCU": winreg.HKEY_CURRENT_USER}[hive]
        with winreg.OpenKey(root, key) as k:
            return str(winreg.QueryValueEx(k, value)[0])
    except (ImportError, OSError):
        return None


def webview2_version(read=_read_registry) -> str | None:
    """Installed WebView2 Runtime version, or None (pv missing or 0.0.0.0)."""
    client = "\\Microsoft\\EdgeUpdate\\Clients\\" + WEBVIEW2_CLIENT
    for hive, key in (("HKLM", "SOFTWARE\\WOW6432Node" + client), ("HKLM", "SOFTWARE" + client),
                      ("HKCU", "Software" + client)):
        v = (read(hive, key, "pv") or "").strip()
        if v and v != "0.0.0.0":
            return v
    return None


def choose_ui(env=os.environ, platform: str = sys.platform, has_webview: bool | None = None,
              webview2: str | None = None) -> str:
    """"web" (pywebview) or "tk" (fallback). NSR_UI=tk|web forces one; on
    Windows the web UI needs the WebView2 Runtime, elsewhere just pywebview."""
    forced = env.get("NSR_UI", "").lower()
    if forced in ("tk", "web"):
        return forced
    if has_webview is None:
        import importlib.util
        has_webview = importlib.util.find_spec("webview") is not None
    if not has_webview:
        return "tk"
    if platform == "win32" and not webview2:
        return "tk"
    return "web"


# --- Launcher: the state machine both views drive ---------------------------------
CONTROLLER_STATES = {"first-import": "synced", "imported": "synced", "updated": "synced",
                     "up-to-date": "synced", "kept-yours": "yours", "no-reborn": "no-reborn",
                     "error": "error"}


class Launcher:
    """Everything behind the buttons, independent of the view.

    Views call the action methods (they return at once; slow work runs via
    spawn) and render snapshot(), a JSON-able dict whose "rev" changes on
    every update. Messages that used to be popups are state["notice"]
    ({id, text, kind}); views show each new id once. spawn/popen/lobby_mod are
    injectable so tests run every flow synchronously without a network or
    Dolphin.
    """

    def __init__(self, folder: str | None = None, spawn=None, popen=None, lobby_mod=None,
                 patcher_mod=None, env=os.environ, loop_spawn=None, stats_post=None):
        self.folder = folder or app_dir()
        self.cfg = load_config(self.folder)
        self._spawn = spawn or (lambda fn: threading.Thread(target=fn, daemon=True).start())
        self._popen = popen or subprocess.Popen
        self._lobby = lobby_mod or lobby
        self._patcher = patcher_mod or patcher
        self._env = env
        self._lock = threading.RLock()
        self._host = None
        self._join_cancel = None
        # Background game-results uploader: its own long-lived thread (tests pass
        # loop_spawn=lambda fn: None and call upload_stats_now directly).
        self._loop_spawn = loop_spawn or (lambda fn: threading.Thread(target=fn, daemon=True).start())
        self._stop = threading.Event()
        self._stats_wake = threading.Event()
        self.uploader = gamestats.Uploader(self.folder, **({"post": stats_post} if stats_post else {}))
        self.rv_server = rendezvous_server(self.folder)
        self.rev = 0
        self._notice_id = 0
        cfg = self.cfg
        self.state = {
            "app": APP_TITLE, "folder": self.folder,
            "name": str(cfg.get("name", env.get("USERNAME", "Player")))[:24],
            "telemetry": bool(cfg.get("telemetry", True)),
            "game_stats": {"enabled": bool(cfg.get("game_stats", True)), "state": "idle",
                           "text": "", "sent": 0, "pending": 0},
            # "rb_delay", not the lockstep-era "delay" (2 by default), so an old
            # config doesn't carry a lockstep value into rollback.
            "delay": self._clamp_delay(cfg.get("rb_delay", DEFAULT_DELAY)),
            "max_delay": MAX_DELAY, "last_host": str(cfg.get("last_host", "")),
            "rooms": bool(self.rv_server),
            "game": {"state": "checking", "text": "Checking your game copy…", "fingerprint": "",
                     "note": "", "progress": None, "busy": False},
            "host": {"state": "idle", "mode": "", "code": "", "players": [], "can_start": False,
                     "info": "" if self.rv_server else "Room codes aren't set up in this download "
                             "(no room server). Use “Host by IP”."},
            "join": {"state": "idle", "info": "", "players": []},
            "controllers": {"state": "checking", "message": "", "source": "",
                            "port1": controllers.port1_summary(self.user_dir), "backup": ""},
            "notice": None,
        }

    # -- state plumbing --
    @property
    def user_dir(self) -> str:
        return os.path.join(self.folder, "User")

    @staticmethod
    def _clamp_delay(value) -> int:
        try:
            return max(0, min(int(value), MAX_DELAY))
        except (TypeError, ValueError):
            return DEFAULT_DELAY

    def _set(self, section: str | None = None, **kw) -> None:
        with self._lock:
            (self.state if section is None else self.state[section]).update(kw)
            self.rev += 1

    def notify(self, text: str, kind: str = "info") -> None:
        with self._lock:
            self._notice_id += 1
            self.state["notice"] = {"id": self._notice_id, "text": text, "kind": kind}
            self.rev += 1

    def snapshot(self, since: int = -1) -> dict | None:
        """Current state (a copy), or None if nothing changed since rev `since`."""
        with self._lock:
            if since == self.rev:
                return None
            snap = json.loads(json.dumps(self.state))
            snap["rev"] = self.rev
            return snap

    def _save(self) -> None:
        save_config(self.folder, self.cfg)

    # -- settings --
    def set_name(self, name: str) -> None:
        name = (name or "").strip()[:24] or "Player"
        self.cfg["name"] = name
        self._save()
        self._set(name=name)

    def set_telemetry(self, on: bool) -> None:
        self.cfg["telemetry"] = bool(on)
        self._save()
        self._set(telemetry=bool(on))

    def set_game_stats(self, on: bool) -> None:
        self.cfg["game_stats"] = bool(on)
        self._save()
        self._set("game_stats", enabled=bool(on))
        if on:
            self._stats_wake.set()

    def set_delay(self, delay) -> None:
        self._set(delay=self._clamp_delay(delay))

    def need_game(self) -> bool:
        if not self.state["game"]["fingerprint"]:
            if self.state["game"].get("busy"):
                self.notify("The game is still getting ready (first start only). One moment…", "info")
            else:
                self.notify("Couldn't find NFL Street Reborn. Settings → Advanced → Locate Reborn.", "error")
            return True
        return False

    # -- game image --
    def _ready(self, fp: str, note: str = "") -> None:
        self._set("game", state="ready", fingerprint=fp, note=note.strip(), progress=None,
                  text="Ready for rollback netplay. Everyone you play with must have the same fingerprint.")

    def check_game(self) -> None:
        def work():
            st = self._patcher.image_status(self.folder)
            if st is None:
                self._set("game", state="missing", fingerprint="",
                          text="Not set up yet. Click “Find Reborn automatically”.")
            elif st.playable:
                self._ready(st.fingerprint, {"release": "", "custom": "Unrecognized Reborn version: "
                                             "compare fingerprints with friends."}.get(
                    st.kind, "Custom game copy (e.g. a ratings pack): everyone needs the identical copy."))
            else:
                self._set("game", state="stale", fingerprint="",
                          text=f"Your game copy ({st.fingerprint}) is {st.note} and can't play this "
                               "version's rollback netplay. Click “Find Reborn automatically” to "
                               "rebuild it (about a minute).")
        self._spawn(work)

    def find_game(self) -> None:
        if self.state["game"]["busy"]:
            return
        self._set("game", text="Looking for NFL Street Reborn…")

        def work():
            iso = self._patcher.find_reborn_iso()
            if iso:
                self.prepare(iso)
            else:
                self._set("game", text="Couldn't find Reborn automatically. Click “Browse for "
                                       "NSR.iso…” and pick content\\games\\NSR.iso in your Reborn folder.")
        self._spawn(work)

    def prepare(self, iso: str) -> None:
        with self._lock:
            if self.state["game"]["busy"]:
                return
            self._set("game", busy=True, state="building", progress=0, text="Checking Reborn's game file…")
        labels = {"check": "Checking Reborn's game file…", "build": "Building the netplay copy…",
                  "verify": "Fingerprinting…"}

        def work():
            try:
                r = self._patcher.prepare_image(iso, self.folder, progress=lambda st, d, t: self._set(
                    "game", text=labels.get(st, st), progress=round(d * 100 / max(t, 1))))
                self.cfg["reborn_iso"] = iso
                self.cfg["reborn_install"] = r.reborn_install
                self._save()
                self._ready(r.fingerprint, "" if r.matches_reference else (
                    "Unrecognized Reborn version: compare fingerprints with friends."
                    if r.matches_reference is None else
                    "WARNING: doesn't match the reference build; ask Mason before playing."))
                self.sync_controllers()  # the install may be new to us: pick up its pad setup
            except isopatch.PatchError:
                self._set("game", state="error", progress=None,
                          text="This Reborn version isn't supported yet (the game code changed). "
                               "Nothing was written. Ask Mason for an updated launcher.")
            except OSError as e:
                self._set("game", state="error", progress=None, text=f"Couldn't build the game copy: {e}")
            finally:
                self._set("game", busy=False)
        self._spawn(work)

    # -- controllers --
    def reborn_install(self, search: bool = False) -> str | None:
        """Reborn's install folder: from the last image build, else (search=True)
        found on disk like NFLStreetRollback-Patcher does."""
        install = self.cfg.get("reborn_install") or self._patcher.saved_reborn_install(self.folder)
        if not install and self.cfg.get("reborn_iso"):
            install = self._patcher.reborn_install_of(self.cfg["reborn_iso"])
        if not install and search:
            iso = self._patcher.find_reborn_iso()
            install = self._patcher.reborn_install_of(iso) if iso else None
        return install if install and os.path.isdir(install) else None

    def sync_controllers_now(self, force: bool = False, search: bool = False) -> controllers.SyncResult:
        source = controllers.find_reborn_source(self.reborn_install(search), self._env)
        res = controllers.sync_controllers(self.user_dir, source, self.cfg.get("controllers"), force)
        if res.record != self.cfg.get("controllers"):
            self.cfg["controllers"] = res.record
            self._save()
        if res.backup:
            log(self.folder, f"controllers: {res.action}; previous mapping saved as {res.backup}")
        self._set("controllers", state=CONTROLLER_STATES.get(res.action, res.action), message=res.message,
                  source=source.gcpad if source else "", backup=res.backup or "",
                  port1=controllers.port1_summary(self.user_dir))
        return res

    def sync_controllers(self, force: bool = False) -> None:
        def work():
            res = self.sync_controllers_now(force=force, search=True)
            if force:
                ok = res.action in ("imported", "first-import", "updated", "up-to-date")
                self.notify(("Imported your Reborn controller settings."
                             + (" Your previous mapping was saved next to it." if res.backup else ""))
                            if ok else res.message, "info" if ok else "error")
        self._spawn(work)

    def import_controllers(self) -> None:
        self.sync_controllers(force=True)

    def refresh_controllers(self) -> None:
        self.sync_controllers(force=False)

    # -- launching --
    def launch(self, start, open_ui: bool = False) -> bool:
        if not open_ui and self.need_game():
            return False
        try:  # never let a controller-file problem block a match
            self.sync_controllers_now()
        except Exception as e:  # noqa: BLE001
            log(self.folder, f"controller sync failed: {e!r}")
        if start is not None and getattr(start, "telemetry_url", ""):
            # Remember the server (and this room's token as registration proof)
            # so Reborn-only games can be uploaded later too.
            self.uploader.remember_server(start.telemetry_url, start.telemetry_token)
            self._stats_wake.set()
        args, env = dolphin_command(self.folder, start, self.reborn_install(), open_ui=open_ui,
                                    name=self.state["name"], fingerprint=self.state["game"]["fingerprint"],
                                    telemetry=self.state["telemetry"])
        if not open_ui:  # same values the fork logs in nsr-p*.log ("local pad:")
            adapter = env.get("NSR_PAD_ADAPTER")
            log(self.folder, f"controller ({'netplay' if start else 'offline'}): "
                f"kind={env.get('NSR_PAD_KIND', '?')} device={env.get('NSR_PAD_DEVICE') or '-'}"
                + (f" usb-adapter-port={int(adapter) + 1}" if adapter else ""))
        try:
            # The fork truncates this per-player log at connection setup. Clear
            # it here too so a failed launch cannot surface an older mismatch.
            if start is not None and not open_ui:
                with open(env["NSR_LOG"], "w", encoding="utf-8"):
                    pass
            process = self._popen(args, env=env, cwd=self.folder)
        except OSError as e:
            self.notify(f"Couldn't start Dolphin ({args[0]}): {e}", "error")
            return False
        if start is not None and not open_ui and hasattr(process, "poll"):
            self._spawn(lambda: self._watch_boot_sync(process, env["NSR_LOG"]))
        return True

    def _watch_boot_sync(self, process, path: str) -> None:
        """Surface the fork's named preboot failure without exposing wire internals.

        Stop watching as soon as the boot barrier passes. Gameplay and Dolphin's
        later exit status belong to its normal UI, not this connection watcher.
        """
        deadline = time.monotonic() + 130  # rendezvous + peer-connect windows
        while time.monotonic() < deadline:
            try:
                with open(path, encoding="utf-8", errors="replace") as f:
                    lines = f.read(65536).splitlines()
            except OSError:
                lines = []
            for line in lines:
                if "boot sync mismatch" in line or "boot sync failed BEFORE boot" in line:
                    self.notify(line.removeprefix("[NSR] "), "error")
                    return
                if "boot sync PASSED" in line:
                    return
            if process.poll() is not None:
                return
            time.sleep(0.2)

    def play_offline(self) -> None:
        self.launch(None)

    def setup_controllers(self) -> None:
        self.launch(None, open_ui=True)

    # -- host --
    def _roster(self, names) -> None:
        self._set("host", players=list(names), can_start=len(names) > 1)

    def host_room(self) -> None:
        if self.need_game() or self._host or self.state["host"]["state"] != "idle":
            return
        if not self.rv_server:
            self.notify("Room codes aren't set up in this download. Use “Host by IP”.", "error")
            return
        self._set("host", state="opening", mode="room", info="Creating a room…")
        h = self._lobby.RoomHost(self.rv_server, self.state["name"], self.state["game"]["fingerprint"],
                                 on_change=self._roster, on_error=lambda why: self._set("host", info=why))

        def work():
            try:
                code = h.open()
            except ConnectionError as e:
                self._set("host", state="idle", mode="", info=f"{e}\nYou can still host by IP address.")
                return
            self._host = h
            self._set("host", state="open", code=code,
                      info="Room open. Give your friends this code; click Start when everyone's listed.")
        self._spawn(work)

    def host_ip(self) -> None:
        if self.need_game() or self._host or self.state["host"]["state"] != "idle":
            return
        try:
            h = self._lobby.LobbyHost(self.state["name"], self.state["game"]["fingerprint"],
                                      on_change=self._roster,
                                      on_reject=lambda who, why: self._set("host", info=f"Refused {who}: {why}"))
            h.open()
        except OSError as e:
            self.notify(f"Couldn't open the lobby on port {self._lobby.PORT}: {e}", "error")
            return
        self._host = h
        lan = lan_address()
        self._set("host", state="open", mode="ip", code=lan,
                  info=f"Lobby open (by IP). Same network? Friends join with:  {lan}\n"
                       "Finding your internet address…")

        def find_public():
            pub = public_address()
            self._set("host", info=(
                f"Lobby open (by IP). Friends join with your address:\n"
                f"Internet:  {pub}      Same network:  {lan}\n"
                f"For internet play, forward TCP and UDP port {self._lobby.PORT} on your router to this PC.")
                if pub else f"Lobby open (by IP). Same network? Friends join with:  {lan}\n"
                            "(Couldn't look up your internet address.)")
        self._spawn(find_public)

    def start_game(self, delay=None) -> None:
        h = self._host
        if not h or self.state["host"]["state"] != "open":
            return
        if delay is not None:
            self.set_delay(delay)
        delay = self.state["delay"]
        self._set("host", state="starting")

        def work():
            try:
                me = h.start(delay=delay)
            except (ValueError, ConnectionError) as e:
                self._set("host", state="open")
                self.notify(str(e), "error")
                return
            self.cfg["rb_delay"] = delay
            self._save()
            self._host = None
            self._set("host", state="idle", mode="", code="", players=[], can_start=False,
                      info=f"Started a {me.players}-player game. Have fun!")
            self.launch(me)
        self._spawn(work)

    def close_room(self) -> None:
        if self._host:
            self._host.close()
            self._host = None
        self._set("host", state="idle", mode="", code="", players=[], can_start=False, info="Closed.")

    # -- join --
    def join(self, target: str) -> None:
        if self.need_game() or self.state["join"]["state"] != "idle":
            return
        target = (target or "").strip()
        if not target:
            self._set("join", info="Type the room code from the host.")
            return
        direct = looks_like_ip(target)
        if not direct and not self.rv_server:
            self._set("join", info="Room codes aren't set up in this download. Ask the host for "
                                   "their IP address instead.")
            return
        self.cfg["name"] = self.state["name"]
        self.cfg["last_host"] = target
        self._save()
        cancel = threading.Event()
        self._join_cancel = cancel
        self._set(last_host=target)
        self._set("join", state="waiting", players=[], info=f"Joining {target}… waiting for the host to start.")
        on_lobby = lambda names: self._set("join", players=list(names))  # noqa: E731

        def work():
            try:
                if direct:
                    start = self._lobby.join(target, self.state["name"], self.state["game"]["fingerprint"],
                                             on_lobby=on_lobby, cancel=cancel)
                else:
                    start = self._lobby.join_room(self.rv_server, target, self.state["name"],
                                                  self.state["game"]["fingerprint"],
                                                  on_lobby=on_lobby, cancel=cancel)
            except ConnectionError as e:
                self._set("join", state="idle", info=str(e))
                return
            self._set("join", state="idle",
                      info=f"Starting! You're player {start.player + 1} of {start.players}.")
            self.launch(start)
        self._spawn(work)

    def leave(self) -> None:
        if self._join_cancel:
            self._join_cancel.set()

    # -- game results upload --
    def upload_stats_now(self, force: bool = False) -> dict:
        """One upload pass (the background loop calls this every few minutes)."""
        if not self.state["game_stats"]["enabled"]:
            self._set("game_stats", state="off", text="Off: nothing is uploaded.")
            return {"state": "off"}
        fallback = telemetry_config(self.folder)
        url = gamestats.games_url(self._env.get("NSR_GAMES_URL", "") or fallback["url"])
        dirs = gamestats.stats_dirs(self.reborn_install(), self._env, own_user_dir=self.user_dir)
        try:
            res = self.uploader.run_once(dirs, self.state["name"], url=url, shared_key=fallback["key"],
                                         force=force)
        except Exception as e:  # noqa: BLE001  (never let uploads break the launcher)
            log(self.folder, f"game stats upload failed: {e!r}")
            return {"state": "error"}
        text = {"ok": f"{res['total_sent']} game result files sent"
                      + (f", {res['pending']} waiting" if res["pending"] else "") + ".",
                "no-server": "Waiting for the server address: it arrives with your first room "
                             "(or nsr-telemetry.txt).",
                "no-token": "The server hasn't accepted this PC yet; play one room through "
                            "room codes and it will.",
                "unauthorized": "The server refused the upload key; it will ask for a new one.",
                "offline": "Server unreachable; will retry.", "waiting": "Will retry shortly.",
                }.get(res["state"], f"Upload problem ({res['state']}); will retry.")
        if not dirs:
            text = "No Reborn game results folder found yet. " + text
        self._set("game_stats", state=res["state"], text=text, sent=res["total_sent"], pending=res["pending"])
        if res.get("sent"):
            log(self.folder, f"game stats: sent {res['sent']}, {res['pending']} pending")
        return res

    def _stats_loop(self) -> None:
        while not self._stop.is_set():
            self.upload_stats_now()
            self._stats_wake.wait(gamestats.INTERVAL_S)
            self._stats_wake.clear()

    def _startup_checks(self) -> None:
        self.check_game()
        # One-click start: a missing or outdated game copy is built right away
        # from the Reborn install we can find (what "Find Reborn automatically"
        # does), so players never need to run the separate patcher first. If
        # Reborn can't be found, the Game card still asks them to browse.
        game = self.state["game"]
        if game.get("state") in ("missing", "stale") and not game.get("busy"):
            log(self.folder, f"startup: game copy {game.get('state')}; building it automatically")
            self.find_game()

    def startup(self) -> None:
        self._spawn(self._startup_checks)
        self.sync_controllers()
        self._loop_spawn(self._stats_loop)

    def shutdown(self) -> None:
        self._stop.set()
        self._stats_wake.set()
        if self._host:
            self._host.close()
            self._host = None
        self.leave()


# --- web view (pywebview) ------------------------------------------------------------
def copy_to_clipboard(text: str) -> bool:
    if sys.platform != "win32":
        return False
    try:  # clip.exe reads UTF-16 with a BOM correctly; room codes are ASCII anyway
        subprocess.run(["clip"], input=("﻿" + text).encode("utf-16-le"), check=False,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), timeout=5)
        return True
    except (OSError, subprocess.SubprocessError):
        return False


class WebApi:
    """window.pywebview.api: thin pass-through to Launcher. Only methods are
    public; pywebview exposes public attributes too, so state stays private."""

    def __init__(self, launcher: Launcher):
        self._l = launcher
        self._window = None
        self._ready = threading.Event()
        self._ready_info: dict = {}

    def snapshot(self, since=-1):
        return self._l.snapshot(since)

    def ui_ready(self, info=None):
        self._ready_info = dict(info or {})
        self._ready.set()

    def set_name(self, name): self._l.set_name(name)
    def set_telemetry(self, on): self._l.set_telemetry(bool(on))
    def set_game_stats(self, on): self._l.set_game_stats(bool(on))
    def set_delay(self, delay): self._l.set_delay(delay)
    def find_game(self): self._l.find_game()
    def host_room(self): self._l.host_room()
    def host_ip(self): self._l.host_ip()
    def start_game(self, delay=None): self._l.start_game(delay)
    def close_room(self): self._l.close_room()
    def join(self, target): self._l.join(target)
    def leave(self): self._l.leave()
    def play_offline(self): self._l.play_offline()
    def setup_controllers(self): self._l.setup_controllers()
    def import_controllers(self): self._l.import_controllers()
    def refresh_controllers(self): self._l.refresh_controllers()
    def copy_text(self, text): return copy_to_clipboard(str(text))

    def browse_iso(self):
        import webview
        kind = getattr(getattr(webview, "FileDialog", None), "OPEN", None) or webview.OPEN_DIALOG
        try:  # pywebview filters must be "Label (*.ext)"; NSR.iso sits in content\games
            picked = self._window.create_file_dialog(kind, file_types=("Disc images (*.iso)",))
        except Exception:  # noqa: BLE001  (a pywebview that rejects the filter)
            picked = self._window.create_file_dialog(kind)
        if picked:
            self._l.prepare(picked[0] if isinstance(picked, (list, tuple)) else picked)


def run_web(launcher: Launcher, selftest: bool = False) -> int:
    """Run the web UI. Returns the exit code; raises if pywebview/WebView2 fails
    so run_gui can fall back to tkinter."""
    import webview
    with open(resource_path(UI_HTML), encoding="utf-8") as f:
        html = f.read()
    api = WebApi(launcher)
    window = webview.create_window(APP_TITLE, html=html, js_api=api, width=1200, height=780,
                                   min_size=(980, 660), background_color="#0b0b0e")
    api._window = window
    result = {"code": 0}

    def probe():  # selftest: the page must render a real snapshot, then close
        ok = api._ready.wait(60)
        info = api._ready_info
        result["code"] = 0 if ok and info.get("rendered") and info.get("pages", 0) >= 6 else 1
        print(f"selftest web ui: ready={ok} info={info}", flush=True)
        log(launcher.folder, f"selftest web ui: ready={ok} info={info}")
        window.destroy()

    launcher.startup()
    webview.start(probe if selftest else None, gui="edgechromium" if sys.platform == "win32" else None,
                  debug=bool(os.environ.get("NSR_UI_DEBUG")))
    launcher.shutdown()
    if not selftest and not api._ready.is_set():
        raise RuntimeError("web UI never came up")
    return result["code"]


# --- tkinter fallback view -------------------------------------------------------------
def run_tk(launcher: Launcher, selftest: bool = False) -> int:
    """Plain fallback window over the same Launcher (no WebView2 on this PC)."""
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title(APP_TITLE)
    root.minsize(620, 600)
    bg, panel, fg, accent, muted = "#0b0b0e", "#15151a", "#f2f2f2", "#e3262f", "#9b9ba7"
    root.configure(bg=bg)
    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure(".", background=bg, foreground=fg, fieldbackground=panel, font=("Segoe UI", 10))
    style.configure("Panel.TFrame", background=panel)
    style.configure("Panel.TLabel", background=panel, foreground=fg)
    style.configure("Muted.TLabel", background=panel, foreground=muted)
    style.configure("Title.TLabel", background=bg, foreground=fg, font=("Segoe UI", 20, "bold italic"))
    style.configure("Code.TLabel", background=panel, foreground=fg, font=("Segoe UI", 22, "bold"))
    style.configure("Big.TLabel", background=panel, foreground=fg, font=("Segoe UI", 12, "bold"))
    style.configure("Accent.TButton", background=accent, foreground="white", padding=8,
                    font=("Segoe UI", 10, "bold"))
    style.map("Accent.TButton", background=[("active", "#b81b23"), ("disabled", "#4a2226")])
    style.configure("TButton", background="#23232a", foreground=fg, padding=6)
    style.configure("TNotebook", background=bg, borderwidth=0)
    style.configure("TNotebook.Tab", background=panel, foreground=fg, padding=(14, 6))
    style.map("TNotebook.Tab", background=[("selected", accent)])
    style.configure("Horizontal.TProgressbar", background=accent, troughcolor=panel)

    outer = ttk.Frame(root, padding=16)
    outer.pack(fill="both", expand=True)
    ttk.Label(outer, text="NFLSTREET ROLLBACK", style="Title.TLabel").pack(anchor="w")
    status = ttk.Label(outer, text="", foreground=muted)
    status.pack(anchor="w", pady=(0, 8))

    def label(parent, row, style="Panel.TLabel", **grid):
        w = ttk.Label(parent, text="", style=style, wraplength=560, justify="left")
        w.grid(**{"row": row, "column": 0, "columnspan": 4, "sticky": "w", "pady": (4, 4), **grid})
        return w

    tabs = ttk.Notebook(outer)
    tabs.pack(fill="both", expand=True)
    pages = {k: ttk.Frame(tabs, style="Panel.TFrame", padding=12) for k in
             ("Host", "Join", "Controllers", "Settings")}
    for k, f in pages.items():
        tabs.add(f, text=k)

    # Host
    hp = pages["Host"]
    host_code = ttk.Label(hp, text="", style="Code.TLabel")
    host_code.grid(row=0, column=0, columnspan=4, sticky="w")
    host_info = label(hp, 1)
    host_roster = tk.Listbox(hp, height=4, bg=bg, fg=fg, highlightthickness=0, borderwidth=0)
    host_roster.grid(row=2, column=0, columnspan=4, sticky="we", pady=6)
    b_open = ttk.Button(hp, text="Create room", style="Accent.TButton", command=launcher.host_room)
    b_start = ttk.Button(hp, text="Start game", style="Accent.TButton",
                         command=lambda: launcher.start_game(delay_var.get()))
    b_close = ttk.Button(hp, text="Close", command=launcher.close_room)
    b_ip = ttk.Button(hp, text="Host by IP address (advanced)", command=launcher.host_ip)
    for i, b in enumerate((b_open, b_start, b_close, b_ip)):
        b.grid(row=3, column=i, sticky="w", padx=(0, 6))
    delay_var = tk.IntVar(value=launcher.state["delay"])
    ttk.Label(hp, text="Input delay for everyone (frames; 1 suits most games, try 2 if it stutters):",
              style="Muted.TLabel").grid(row=4, column=0, columnspan=3, sticky="w", pady=(10, 0))
    ttk.Spinbox(hp, from_=0, to=MAX_DELAY, textvariable=delay_var, width=4,
                command=lambda: launcher.set_delay(delay_var.get())).grid(row=4, column=3, sticky="w", pady=(10, 0))
    ttk.Button(hp, text="Practice offline", command=launcher.play_offline).grid(row=5, column=0, sticky="w", pady=8)

    # Join
    jp = pages["Join"]
    ttk.Label(jp, text="Room code (or host IP)", style="Panel.TLabel").grid(row=0, column=0, sticky="w")
    addr_var = tk.StringVar(value=launcher.state["last_host"])
    ttk.Entry(jp, textvariable=addr_var, width=30).grid(row=0, column=1, sticky="w", padx=6)
    join_info = label(jp, 1)
    join_roster = tk.Listbox(jp, height=4, bg=bg, fg=fg, highlightthickness=0, borderwidth=0)
    join_roster.grid(row=2, column=0, columnspan=4, sticky="we")
    b_join = ttk.Button(jp, text="Join", style="Accent.TButton", command=lambda: launcher.join(addr_var.get()))
    b_leave = ttk.Button(jp, text="Leave", command=launcher.leave)
    b_join.grid(row=3, column=0, sticky="w", pady=8)
    b_leave.grid(row=3, column=1, sticky="w")

    # Controllers
    cp = pages["Controllers"]
    ctl_dev = label(cp, 1, style="Big.TLabel")
    ctl_msg = label(cp, 2)
    ttk.Label(cp, text="Your controller is always Port 1.", style="Muted.TLabel").grid(row=3, column=0, columnspan=4, sticky="w")
    ttk.Button(cp, text="Use my Reborn controller settings", style="Accent.TButton",
               command=launcher.import_controllers).grid(row=4, column=0, sticky="w", pady=8)
    ttk.Button(cp, text="Set up controllers…", command=launcher.setup_controllers).grid(row=4, column=1, sticky="w", padx=6)
    ctl_binds = label(cp, 5, style="Muted.TLabel")

    # Settings
    sp = pages["Settings"]
    ttk.Label(sp, text="Your name", style="Panel.TLabel").grid(row=0, column=0, sticky="w", pady=4)
    name_var = tk.StringVar(value=launcher.state["name"])
    name_entry = ttk.Entry(sp, textvariable=name_var, width=24)
    name_entry.grid(row=0, column=1, sticky="w")
    name_entry.bind("<FocusOut>", lambda e: launcher.set_name(name_var.get()))
    telemetry_var = tk.BooleanVar(value=launcher.state["telemetry"])
    ttk.Checkbutton(sp, text="Send lag/rollback stats to Mason's server to help fix netplay",
                    variable=telemetry_var, command=lambda: launcher.set_telemetry(telemetry_var.get())) \
        .grid(row=1, column=0, columnspan=4, sticky="w", pady=(6, 0))
    ttk.Label(sp, text="Connection and rollback counters, your name and the room code. Never your button "
                       "presses, files, or game data. Untick to send nothing.", style="Muted.TLabel",
              wraplength=560, justify="left").grid(row=2, column=0, columnspan=4, sticky="w")
    stats_var = tk.BooleanVar(value=launcher.state["game_stats"]["enabled"])
    ttk.Checkbutton(sp, text="Upload game results to Mason's server for the Stats page",
                    variable=stats_var, command=lambda: launcher.set_game_stats(stats_var.get())) \
        .grid(row=3, column=0, columnspan=4, sticky="w", pady=(6, 0))
    ttk.Label(sp, text=GAME_STATS_NOTE, style="Muted.TLabel", wraplength=560, justify="left") \
        .grid(row=4, column=0, columnspan=4, sticky="w")
    stats_text = label(sp, 5, style="Muted.TLabel")
    game_text = label(sp, 6, pady=(14, 4))
    progress = ttk.Progressbar(sp, length=300, mode="determinate")
    b_find = ttk.Button(sp, text="Find Reborn automatically", command=launcher.find_game)
    b_browse = ttk.Button(sp, text="Browse for NSR.iso…")
    b_find.grid(row=8, column=0, sticky="w")
    b_browse.grid(row=8, column=1, sticky="w", padx=6)

    def browse():
        iso = filedialog.askopenfilename(title="Pick NFL Street Reborn's NSR.iso (content\\games)",
                                         filetypes=[("Reborn game image", "NSR.iso"), ("ISO", "*.iso")])
        if iso:
            launcher.prepare(iso)
    b_browse.configure(command=browse)

    def set_list(box, names):
        box.delete(0, "end")
        for i, n in enumerate(names):
            box.insert("end", f"  Player {i + 1}: {n}" + ("  (host)" if i == 0 else ""))

    def enable(btn, on):
        btn.state(["!disabled"] if on else ["disabled"])

    seen = {"rev": -1, "notice": 0, "rendered": False}

    def render():
        s = launcher.snapshot(seen["rev"])
        if s is not None:
            seen["rev"] = s["rev"]
            g, h, j, c = s["game"], s["host"], s["join"], s["controllers"]
            status.configure(text=("● Ready to play" if g["state"] == "ready" else "● " + g["text"])
                             + f"     {s['name']} · Port 1")
            host_code.configure(text=h["code"] if h["state"] in ("open", "starting") else "")
            host_info.configure(text=h["info"])
            set_list(host_roster, h["players"])
            idle = h["state"] == "idle"
            enable(b_open, idle and s["rooms"]); enable(b_ip, idle)
            enable(b_close, not idle); enable(b_start, h["state"] == "open" and h["can_start"])
            join_info.configure(text=j["info"])
            set_list(join_roster, j["players"])
            enable(b_join, j["state"] == "idle"); enable(b_leave, j["state"] == "waiting")
            p1 = c["port1"]
            ctl_dev.configure(text=f"{p1['family']}  ({p1['device']})" if p1["mapped"] else "No controller mapped")
            ctl_msg.configure(text=c["message"] + (f"\nFrom: {c['source']}" if c["source"] else ""))
            ctl_binds.configure(text="   ".join(f"{b['control']}: {b['input'] or '-'}" for b in p1["bindings"]))
            game_text.configure(text=(f"Fingerprint {g['fingerprint']}\n" if g["fingerprint"] else "")
                                + g["text"] + (f"\n{g['note']}" if g["note"] else ""))
            if g["progress"] is None:
                progress.grid_forget()
            else:
                progress.grid(row=7, column=0, columnspan=3, sticky="w", pady=4)
                progress.configure(value=g["progress"])
            enable(b_find, not g["busy"]); enable(b_browse, not g["busy"])
            stats_text.configure(text=s["game_stats"]["text"])
            n = s["notice"]
            if n and n["id"] != seen["notice"]:
                seen["notice"] = n["id"]
                if not selftest:
                    (messagebox.showerror if n["kind"] == "error" else messagebox.showinfo)(APP_TITLE, n["text"])
            seen["rendered"] = True
        root.after(100, render)

    def on_close():
        launcher.shutdown()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    launcher.startup()
    render()
    if selftest:
        root.update()
        root.after(500, on_close)
    root.mainloop()
    if selftest:
        print(f"selftest tk ui: rendered={seen['rendered']}", flush=True)
        return 0 if seen["rendered"] else 1
    return 0


def run_gui(selftest: bool = False, force_tk: bool = False) -> int:
    launcher = Launcher()
    ui = "tk" if force_tk else choose_ui(webview2=webview2_version() if sys.platform == "win32" else None)
    if ui == "web":
        try:
            return run_web(launcher, selftest)
        except Exception as e:  # noqa: BLE001  (no WebView2, broken pythonnet, ...)
            log(launcher.folder, f"web UI unavailable, using the basic window: {e!r}")
            if selftest:
                print(f"selftest web ui failed: {e!r}", flush=True)
                return 1
            launcher = Launcher()
    return run_tk(launcher, selftest)


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    return run_gui(selftest="--selftest" in argv, force_tk="--tk" in argv)


if __name__ == "__main__":
    sys.exit(main())
