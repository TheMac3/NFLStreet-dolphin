#!/usr/bin/env python3
"""nsr_launcher.py: "NFLStreetRollback", the friend-facing launcher for NFLStreet netplay
("NFLStreetRollback.exe"; short form "NSRB"; the module, NSR_* variables and
nsr-*.json/.txt file names keep their old internal names on purpose).

A window that replaces command lines and .bat files. It lives in the
NFLStreet-dolphin folder (next to Dolphin.exe) and handles these workflows:

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
  5. Game results: new per-game stats files our own Dolphin writes
     (User/GameStats/) are uploaded in the background to Mason's
     server for the shared Stats page (nsr_gamestats.py, docs/stats.md),
     unless the player unticks "Upload game results".
  6. Content updates (docs/content-updates.md): on start, before hosting or
     joining, and on "Check for updates", fetch Mason's signed content
     manifest, verify it (nsr_content), download the ratings packs it names
     (our own edit lists, never game data), and stage them for boot-time
     injection by the matching fork. The unedited image stays unchanged.
     A setting preserves the image-rebuild fallback. Offline practice re-verifies
     the installed signed release and applies the same edits.

Reborn is never modified: its files are only read, this Dolphin runs in
portable mode with its own settings (User/ next to it). Offline saves persist;
netplay uses fresh disposable cards and canonical SRAM owned by the fork, and
Reborn's HD textures are imported into our private Assets/ store on first
setup (nsr_textures.py). Subsequent launches use only our local image/assets
and controller Config. Only metadata probes detect source updates; the
Settings → Advanced re-import button explicitly refreshes the local assets.

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
import nsr_content  # noqa: E402
import nsr_lobby as lobby  # noqa: E402
import nsr_patcher as patcher  # noqa: E402
import nsr_textures as textures  # noqa: E402

APP_TITLE = "NFLStreetRollback"  # window title; user-facing product name
CONFIG_NAME = "nsr-launcher.json"
# Rollback input delay, the same for every player (the host's choice is sent
# to all). AUTO_DELAY (the default) lets the host's Dolphin choose 1..4 frames
# at every match start from the measured connection (fork fix/audio2:
# NSR_RB_DELAY=auto, chosen by player 0 from ENet RTT/jitter and adopted by
# every peer in the entry handoff); 1..MAX_DELAY fixes it. On the lobby and
# room-server wire Auto travels as delay 0 (the deployed room server only
# relays integers; 0 = "no delay" is no longer offered).
AUTO_DELAY = 0
DEFAULT_DELAY = AUTO_DELAY
MAX_DELAY = 4
# Saved choice (nsr-launcher.json). "rb_delay" (written by older launchers at
# every start, usually the old default 1) is migrated once: 2..4 means the
# player picked it, so it is kept; anything else becomes Auto.
DELAY_CONFIG_KEY = "rb_delay_choice"
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
# Internal resolution choices (Settings → Graphics). 0 = leave it to our
# Dolphin's own GFX.ini (native 1x unless the player changed it there).
# Presentation only: the fork answers EFB peeks canonically in netplay, so
# peers may differ. Each step costs frame time because single-core Dolphin
# renders on the emulation thread (OGL, k=2 misprediction, RTX 3070 Ti:
# 14.5 ms per live frame at 1x vs 19.9 ms at 3x; local/work/textures/REPORT.md),
# hence no higher default.
RESOLUTIONS = (0, 1, 2, 3, 4)


# --- pure helpers (tested without a GUI) --------------------------------------
def app_dir() -> str:
    if os.environ.get("NSR_APP_DIR"):
        return os.environ["NSR_APP_DIR"]
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.getcwd()


GAME_STATS_NOTE = ("Final scores and box-score stats of the games you finish in NFLStreetRollback (the "
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
                    telemetry: bool = True, quickstart: str = "manual", icons: str = "auto",
                    resolution: int = 0) -> tuple[list[str], dict[str, str]]:
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
    pad = controllers.controller_kind(os.path.join(folder, "User"))
    image_manifest = patcher.read_manifest(folder)
    retail = (image_manifest.get("recipe") == patcher.ownimage_build.RECIPE and
              image_manifest.get("base_family") == patcher.ownimage_build.FAMILY)
    if retail:
        # Refuse legacy/unknown receipts with an actionable error before boot.
        patcher.retail_fingerprint(image_manifest["output_sha256"], image_manifest.get("variant", ""))
        reborn_install = None
        env.update({"NSR_IMAGE_FAMILY": image_manifest["base_family"],
                    "NSR_IMAGE_RECIPE": image_manifest["recipe"],
                    "NSR_IMAGE_VARIANT": image_manifest["variant"],
                    "NSR_IMAGE_SHA256": image_manifest["output_sha256"]})
    # Preserve the newer launcher-owned asset store for Reborn. Retail
    # always uses an empty overlay and clears any previously imported list.
    tex = textures.TextureSet() if retail else textures.compose(folder, icons, pad.kind)
    textures.write_list(os.path.join(folder, "User"), tex)
    env["NSR_TEXTURES"] = tex.summary()  # logged by the launcher; the fork ignores it
    if tex.warnings:
        env["NSR_TEXTURES_WARNING"] = " ".join(tex.warnings)
    args += ["-C", f"Graphics.Settings.HiresTextures={not retail}",
             "-C", "Graphics.Settings.CacheHiresTextures=False"]
    if resolution in RESOLUTIONS[1:]:
        args += ["-C", f"Graphics.Settings.InternalResolution={resolution}"]
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
    env.update(pad.env())
    # Per-match RNG seed: agreed in the lobby for netplay; fresh per launch
    # offline, so Quick Play differs every time either way.
    env["NSR_SEED"] = str(start.seed if start else secrets.randbits(32))
    # Room selection wins on every peer; practice uses the same one-shot macro.
    selected = lobby.quickstart_id(start.quickstart if start else quickstart)
    if retail and selected == "pickup":
        raise ValueError("Retail Pickup navigation is not qualified yet; choose Quick Play or Manual navigation.")
    if selected != "manual":
        env["NSR_QUICKSTART"] = selected
    if start:
        env.update({
            "NSR_NETPLAY": "1",
            # Compatibility is independent of the optional telemetry upload.
            "NSR_FINGERPRINT": fingerprint or "",
            "NSR_PLAYER": str(start.player),
            "NSR_PLAYERS": str(start.players),
            "NSR_PORT": str(lobby.PORT),
            "NSR_TOPOLOGY": "star",
            "NSR_DELAY": str(start.delay or MENU_LOCKSTEP_DELAY),  # lockstep mode: Auto = 2
            "NSR_LOG": os.path.join(folder, f"nsr-p{start.player}.log"),
        })
        if start.mode == "rollback":
            # Stage 4 keeps menus in lockstep and agrees match boundaries on
            # the frame-keyed channel, so an operator-supplied epoch is both
            # unnecessary and unsafe here.
            env.update(ROLLBACK_ENV)
            env["NSR_RB_DELAY"] = "auto" if start.delay == AUTO_DELAY else str(start.delay)
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
CONTENT_FILE = "nsr-content.txt"  # optional, shipped next to the launcher: content base URL
# A start/Join re-checks for content updates only if the last check is older
# than this, so joining doesn't wait on the network every time.
CONTENT_RECHECK_S = 300


def content_url(folder: str, env=os.environ, games_url: str = "") -> str:
    """Base URL for content updates (".../v1/content"): NSR_CONTENT_URL, else
    nsr-content.txt next to the launcher, else the telemetry service a room
    told us about (games_url) or nsr-telemetry.txt's url, whose /v1/telemetry
    or /v1/games suffix becomes /v1/content (same service, same host).
    https only (loopback http for development); "" when none is known."""
    candidates = [env.get("NSR_CONTENT_URL", "")]
    try:
        with open(os.path.join(folder, CONTENT_FILE)) as f:
            candidates.append(f.readline())
    except OSError:
        pass
    candidates += [games_url, telemetry_config(folder)["url"]]
    for c in candidates:
        c = (c or "").strip().rstrip("/")
        for suffix in ("/v1/telemetry", "/v1/games"):
            if c.endswith(suffix):
                c = c[: -len(suffix)] + "/v1/content"
        url = nsr_content.check_base_url(c)
        if url:
            return url
    return ""


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
                 patcher_mod=None, env=os.environ, loop_spawn=None, stats_post=None,
                 content_fetch=None, content_keys=None, clock=time.monotonic):
        self.folder = folder or app_dir()
        self.cfg = load_config(self.folder)
        self._spawn = spawn or (lambda fn: threading.Thread(target=fn, daemon=True).start())
        self._popen = popen or subprocess.Popen
        self._lobby = lobby_mod or lobby
        self._patcher = patcher_mod or patcher
        self._env = env
        self._lock = threading.RLock()
        self._textures_logged = False
        self._host = None
        self._join_cancel = None
        # Background game-results uploader: its own long-lived thread (tests pass
        # loop_spawn=lambda fn: None and call upload_stats_now directly).
        self._loop_spawn = loop_spawn or (lambda fn: threading.Thread(target=fn, daemon=True).start())
        self._stop = threading.Event()
        self._stats_wake = threading.Event()
        self.uploader = gamestats.Uploader(self.folder, **({"post": stats_post} if stats_post else {}))
        self.rv_server = rendezvous_server(self.folder)
        # Content updates: fetch(url, limit) -> bytes and the trusted keys are
        # injectable so tests run the whole flow against a fake server.
        self._content_fetch = content_fetch or nsr_content.http_get
        self._content_keys = content_keys
        self._content_cache = nsr_content.Cache(self.folder, content_keys)
        self._clock = clock
        self._checked_at = None   # clock() of the last successful manifest check
        self._runtime_content = None  # installed, signed runtime release (also used offline)
        self._source_sha = ""     # SHA-256 of the Reborn image nsr-net.iso was built from
        self._proc = None         # the Dolphin we started, to avoid rebuilding under it
        self.rev = 0
        self._notice_id = 0
        cfg = self.cfg
        import draftmodes
        order, first = cfg.get("draft_order", "alternating"), cfg.get("draft_first", "stock")
        order = order if order in draftmodes.ORDERS else "alternating"
        first = first if first in draftmodes.FIRSTS else "stock"
        self.state = {
            "app": APP_TITLE, "folder": self.folder,
            "name": str(cfg.get("name", env.get("USERNAME", "Player")))[:24],
            "telemetry": bool(cfg.get("telemetry", True)),
            "game_stats": {"enabled": bool(cfg.get("game_stats", True)), "state": "idle",
                           "text": "", "sent": 0, "pending": 0},
            # Not the lockstep-era "delay" (2 by default), so an old config
            # doesn't carry a lockstep value into rollback. 0 = Auto.
            "delay": self._saved_delay(cfg),
            "quickstart": (cfg.get("quickstart") if isinstance(cfg.get("quickstart"), str)
                           and cfg["quickstart"] in lobby.QUICKSTARTS else "manual"),
            "game_modes": lobby.QUICKSTARTS,
            "max_delay": MAX_DELAY, "last_host": str(cfg.get("last_host", "")),
            "rooms": bool(self.rv_server),
            "game": {"state": "checking", "text": "Checking your game copy…", "fingerprint": "",
                     "note": "", "progress": None, "busy": False, "content": None,
                     "draft_order": order, "draft_first": first},
            # Content updates. state: idle|off|checking|downloading|updating|current|
            # available|offline|blocked|error. version: installed (0 = none applied),
            # latest: newest verified on the server.
            "content": {"state": "idle", "text": "", "version": None, "latest": None, "changelog": "",
                        "auto": bool(cfg.get("content_auto", True)), "busy": False,
                        "runtime": bool(cfg.get("content_runtime", True))},
            "host": {"state": "idle", "mode": "", "code": "", "players": [], "can_start": False,
                     "info": "" if self.rv_server else "Room codes aren't set up in this download "
                             "(no room server). Use “Host by IP”."},
            "join": {"state": "idle", "info": "", "players": [], "quickstart": "manual", "draft_label": ""},
            "controllers": {"state": "checking", "message": "", "source": "",
                            "port1": controllers.port1_summary(self.user_dir), "backup": ""},
            # Button icons (nsr_textures): the setting (auto|gamecube|playstation|
            # xbox), what a launch would use now, and the texture source summary.
            "textures": {"icons": self._icons_setting(), "effective": "", "why": "",
                         "base_kind": "", "base": "", "packages": [], "warning": ""},
            "resolution": self._resolution_setting(),
            "notice": None,
        }

    # -- state plumbing --
    @property
    def user_dir(self) -> str:
        return os.path.join(self.folder, "User")

    @staticmethod
    def _clamp_delay(value) -> int:
        """0 (Auto) or 1..MAX_DELAY; "auto" and anything unreadable mean Auto."""
        try:
            return max(0, min(int(value), MAX_DELAY))
        except (TypeError, ValueError):
            return DEFAULT_DELAY

    @classmethod
    def _saved_delay(cls, cfg: dict) -> int:
        if DELAY_CONFIG_KEY in cfg:
            return cls._clamp_delay(cfg[DELAY_CONFIG_KEY])
        old = cls._clamp_delay(cfg.get("rb_delay", AUTO_DELAY))  # pre-Auto launchers
        return old if old >= 2 else AUTO_DELAY

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

    def _icons_setting(self) -> str:
        v = self.cfg.get("button_icons", "auto")
        return v if v in textures.ICON_CHOICES else "auto"

    def _resolution_setting(self) -> int:
        try:
            v = int(self.cfg.get("resolution", 0))
        except (TypeError, ValueError):
            return 0
        return v if v in RESOLUTIONS else 0

    def set_button_icons(self, choice: str) -> None:
        """Button icons: auto (from the controller) or a fixed set. Per PC only."""
        self.cfg["button_icons"] = choice if choice in textures.ICON_CHOICES else "auto"
        self._save()
        self._set("textures", icons=self._icons_setting())
        self._spawn(self.refresh_textures)

    def set_resolution(self, value) -> None:
        try:
            value = int(value)
        except (TypeError, ValueError):
            value = 0
        self.cfg["resolution"] = value if value in RESOLUTIONS else 0
        self._save()
        self._set(resolution=self._resolution_setting())

    def refresh_textures(self) -> textures.TextureSet:
        """What the next launch will show (nothing is written here)."""
        pad = controllers.controller_kind(self.user_dir)
        retail = patcher.read_manifest(self.folder).get("base_family") == patcher.ownimage_build.FAMILY
        ts = textures.TextureSet() if retail else textures.compose(self.folder, self._icons_setting(), pad.kind)
        self._set("textures", icons=self._icons_setting(), effective=ts.icons, why=ts.icons_why,
                  base_kind=ts.base_kind, base=ts.base or "",
                  packages=[f"{p.id} {p.version}" for p in ts.packages],
                  warning=" ".join(ts.warnings))
        return ts

    def need_game(self) -> bool:
        if self.state["game"].get("busy"):
            self.notify("The game is still getting ready. One moment…", "info")
            return True
        if not self.state["game"]["fingerprint"]:
            self.notify(self.state["game"].get("text") or
                        "Couldn't find NFL Street Reborn. Settings → Advanced → Locate Reborn.", "error")
            return True
        return False

    # -- game image --
    def _ready(self, fp: str, note: str = "", content: dict | None = None) -> None:
        self._set("game", state="ready", fingerprint=fp, note=note.strip(), progress=None, content=content,
                  text="Ready for rollback netplay. Everyone you play with must have the same fingerprint.")
        self._set("game", room_fingerprint=self.room_fingerprint())
        self._set("content", version=content["version"] if content else None)

    def draft_settings(self) -> dict:
        """Snapshot the host/practice choice; joiners use their room StartInfo instead."""
        g = self.state["game"]
        return {"order": g["draft_order"], "first": g["draft_first"]}

    def room_fingerprint(self, selected=None) -> str:
        """Label the launch with its agreed policy; M2 compares the full boot bundle."""
        import draftmodes
        g = self.state["game"]
        selected = lobby.draft_settings(selected) if selected is not None else self.draft_settings()
        return draftmodes.room_fingerprint(g["fingerprint"], selected["order"], selected["first"])

    def set_draft(self, order: str, first: str) -> None:
        """Persist the next host/practice room choice without rebuilding the base image."""
        import draftmodes
        draftmodes.patch_names(order, first)
        with self._lock:
            if (self.state["game"]["busy"] or self.state["content"]["busy"] or self._dolphin_running()
                    or self.state["host"]["state"] != "idle" or self.state["join"]["state"] != "idle"):
                self.notify("Close the game, leave your room and wait for updates before changing draft setup.", "error")
                return
            self.cfg.update(draft_order=order, draft_first=first)
            self._save()
            self._set("game", draft_order=order, draft_first=first,
                      note=f"Pickup draft: {order}; first: {first}. Shared with joiners; applied at Select Controller and the draft.")
            self._set("game", room_fingerprint=self.room_fingerprint())

    def _check_game_now(self) -> None:
        """Restore the real image identity, content and matching draft manifest."""
        st = self._patcher.image_status(self.folder)
        if st is None:
            self._set("game", state="missing", fingerprint="", content=None,
                      text="Not set up yet. Click “Find Reborn automatically”.")
        elif st.playable:
            self._source_sha = getattr(st, "source_sha256", "") or ""
            self._ready(st.fingerprint, {"release": "", "retail": st.note, "content": st.note, "custom": st.note + "; compare fingerprints with friends."}.get(
                st.kind, "Custom game copy (e.g. a ratings pack): everyone needs the identical copy."),
                getattr(st, "content", None))
            try:
                self._runtime_content = self._installed_runtime()
                if self._runtime_content:
                    self._validate_runtime(self._runtime_content)
                    self._ready(st.fingerprint, "Ratings are applied when the game starts.",
                                self._runtime_content.descriptor())
            except (OSError, nsr_content.ContentError) as exc:
                self._set("game", state="stale", fingerprint="", content=None,
                          text=f"Installed runtime content could not be verified: {exc}")
        else:
            self._set("game", state="stale", fingerprint="", content=None,
                      text=f"Your game copy ({st.fingerprint}) is {st.note} and can't play this "
                           "version's rollback netplay. Click “Find Reborn automatically” to "
                           "rebuild it (about a minute).")

    def check_game(self) -> None:
        self._spawn(self._check_game_now)

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

    def _claim_build(self, text: str) -> dict | None:
        """Mark the game copy busy (one build at a time). Returns the previous
        game state to restore if a content update fails, or None if busy."""
        with self._lock:
            if self.state["game"]["busy"]:
                return None
            before = dict(self.state["game"])
            self._set("game", busy=True, state="building", fingerprint="", progress=0, text=text)
            return before

    def _run_build(self, iso: str, content=None, strict: bool = False, restore: dict | None = None) -> bool:
        """Build nsr-net.iso (with a content update if given) after _claim_build.
        On failure the previous nsr-net.iso is untouched (prepare_image builds
        into a temporary file); with restore, the game state goes back to it
        too, because that copy is still playable."""
        labels = {"check": "Checking the game file…",
                  "build": "Building the netplay copy…" if not content else
                           f"Building the netplay copy with game content v{content.version}…",
                  "verify": "Fingerprinting…"}
        built = False
        runtime = content if self.state["content"]["runtime"] else None
        extra = {"content": content, "content_strict": strict} if content and not runtime else {}
        order, first = self.state["game"]["draft_order"], self.state["game"]["draft_first"]
        try:
            r = self._patcher.prepare_image(iso, self.folder, progress=lambda st, d, t: self._set(
                "game", text=labels.get(st, st), progress=round(d * 100 / max(t, 1))), **extra)
            built = True
            self.cfg["reborn_iso"] = iso
            self.cfg["reborn_install"] = r.reborn_install
            self._save()
            self._source_sha = getattr(r, "source_sha256", "") or self._source_sha
            applied = getattr(r, "content", None)
            descriptor = applied.descriptor() if applied else (
                nsr_content.content_descriptor(0, 0)
                if r.matches_reference or getattr(r, "base_content_known", False) else None)
            note = f"Pickup draft: {order}; first: {first}. " + ("" if r.matches_reference else (
                ("Custom draft build: compare fingerprints with friends." if r.reborn_label else
                 "Unrecognized Reborn version: compare fingerprints with friends.")
                if r.matches_reference is None else
                "WARNING: doesn't match the reference build; ask Mason before playing."))
            if r.fingerprint.startswith("retail:"):
                note = "Native rosters and controls; " + patcher.read_manifest(self.folder)["variant"]
            if runtime:
                self._validate_runtime(runtime, r.fingerprint)
                self._activate_runtime(runtime)
                descriptor = runtime.descriptor()
                note = "Ratings are applied when the game starts."
            else:
                self._content_cache.clear_runtime()
                self.cfg.pop("runtime_installed", None)
                self._save()
                self._runtime_content = None
            if content and not applied and not runtime:
                note = (note + f"\nGame content v{content.version} doesn't support this Reborn version yet; "
                               "built without it.").strip()
            self.sync_controllers_now()
            self._ensure_assets(install=r.reborn_install)
            self._ready(r.fingerprint, note, descriptor)
            return True
        except textures.ImportError as e:
            # The ISO may already have been published. Retain it on disk, but
            # do not call a first setup complete with only half its assets.
            if textures.read_store(self.folder):
                self._check_game_now()
                self.notify(f"{e} Your existing local textures are kept.", "error")
            else:
                self._set("game", state="error", fingerprint="", progress=None, text=str(e))
            log(self.folder, f"local texture import: {e}")
        except isopatch.PatchError as exc:
            self._fail_build(restore, f"Could not build this source: {exc}. Your previous image was kept.")
        except nsr_content.ContentError as e:
            log(self.folder, f"content build failed: {e}")
            self._fail_build(restore, f"The game content update didn't apply: {e}", recheck=built)
        except (OSError, ValueError) as e:
            self._fail_build(restore, f"Couldn't build the game copy: {e}"
                             + (" (Is the game still running?)" if isinstance(e, PermissionError) else ""), recheck=built)
        finally:
            self._set("game", busy=False)
        return False

    def _fail_build(self, restore: dict | None, text: str, recheck: bool = False) -> None:
        """Restore UI only while prepare_image kept the prior image. If a later
        runtime activation fails, re-read actual disk state before allowing play."""
        if recheck:
            self._check_game_now()
            self.notify(text + " The base image was rebuilt; check Game copy before playing.", "error")
        elif restore:
            restore = {k: v for k, v in restore.items() if k != "busy"}
            self._set("game", **{**restore, "progress": None})
            self.notify(text + " Your previous game copy was kept.", "error")
        else:
            self._set("game", state="error", progress=None, text=text)

    def prepare(self, iso: str) -> None:
        """Build the game copy from a Reborn image the player picked/found,
        with the newest verified content update when one is known."""
        with self._lock:
            if (self._dolphin_running() or self.state["content"]["busy"]
                    or self.state["host"]["state"] != "idle" or self.state["join"]["state"] != "idle"):
                self.notify("Close the game, leave your room and wait for updates before rebuilding.", "error")
                return
            before = self._claim_build("Checking Reborn's game file…")
            if before is None:
                return
        self._spawn(lambda: self._run_build(iso, self._known_content(), strict=False, restore=before))

    # -- controllers --
    def reborn_install(self, search: bool = False) -> str | None:
        """Reborn's install folder: from the last image build, else (search=True)
        found on disk like NFLStreetRollback-Patcher does."""
        if patcher.read_manifest(self.folder).get("base_family") == patcher.ownimage_build.FAMILY:
            return None
        install = self.cfg.get("reborn_install") or self._patcher.saved_reborn_install(self.folder)
        if not install and self.cfg.get("reborn_iso"):
            install = self._patcher.reborn_install_of(self.cfg["reborn_iso"])
        if not install and search:
            iso = self._patcher.find_reborn_iso()
            install = self._patcher.reborn_install_of(iso) if iso else None
        return install if install and os.path.isdir(install) else None

    def sync_controllers_now(self, force: bool = False, search: bool = False) -> controllers.SyncResult:
        # Once imported (or mapped in our Dolphin), our Config owns controls.
        # Only the explicit import button may read Reborn again.
        record = self.cfg.get("controllers") or {}
        own_pad = os.path.join(self.user_dir, "Config", "GCPadNew.ini")
        retail = patcher.read_manifest(self.folder).get("base_family") == patcher.ownimage_build.FAMILY
        if retail:
            source = None
            res = controllers.SyncResult("kept-yours", record, "Using native Dolphin controller settings.")
        elif not force and (record.get("written") or os.path.isfile(own_pad)
                          or textures.read_store(self.folder, validate=False)):
            source = None
            synced = record.get("written") == controllers.text_hash(controllers.read_text(own_pad) or "")
            res = controllers.SyncResult("up-to-date" if synced else "kept-yours", record,
                                         "Using your saved NFLStreetRollback controller settings.")
        else:
            source = controllers.find_reborn_source(self.reborn_install(search), self._env)
            res = controllers.sync_controllers(self.user_dir, source, record, force)
        if res.record != self.cfg.get("controllers"):
            self.cfg["controllers"] = res.record
            self._save()
        if res.backup:
            log(self.folder, f"controllers: {res.action}; previous mapping saved as {res.backup}")
        self._set("controllers", state=CONTROLLER_STATES.get(res.action, res.action), message=res.message,
                  source=source.gcpad if source else "", backup=res.backup or "",
                  port1=controllers.port1_summary(self.user_dir))
        try:  # Auto button icons follow the (possibly just imported) controller
            self.refresh_textures()
        except Exception as e:  # noqa: BLE001
            log(self.folder, f"texture refresh failed: {e!r}")
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

    # -- content updates --
    def content_base(self) -> str:
        return content_url(self.folder, self._env, self.uploader.state.get("url", ""))

    def set_content_runtime(self, on: bool) -> None:
        """Choose the delivery path for the next update; the installed release
        stays active until Check for updates completes the switch safely."""
        self.cfg["content_runtime"] = bool(on)
        self._save()
        self._set("content", runtime=bool(on), text="Click Check for updates to apply this setting.")

    def _installed_runtime(self):
        """A separately persisted identity detects deletion or replacement of the
        signed installed pointer, including between room entry and launch."""
        active = self._content_cache.runtime_content()
        expected = self.cfg.get("runtime_installed")
        if expected:
            if active is None or expected != self._runtime_identity(active):
                raise nsr_content.ContentError("Installed runtime content is missing or changed. Check for updates or rebuild the base.")
        return active

    @staticmethod
    def _runtime_identity(content) -> dict:
        """Only metadata is duplicated; trust still comes from the signed file."""
        import hashlib
        return {"version": content.version, "sha256": hashlib.sha256(content.runtime_bytes()).hexdigest()}

    def _activate_runtime(self, content) -> None:
        """Publish the signed pointer first, then its expected identity. A crash
        between them blocks launch rather than silently selecting other ratings."""
        self._content_cache.activate_runtime(content)
        self.cfg["runtime_installed"] = self._runtime_identity(content)
        # Installation metadata must not use the legacy best-effort settings
        # writer: a failed save must be visible, so missing packs cannot later
        # be mistaken for an intentional stock installation.
        nsr_content._atomic_write(os.path.join(self.folder, CONFIG_NAME),
                                  json.dumps(self.cfg, indent=1).encode("utf-8"))
        self._runtime_content = content

    def _validate_runtime(self, content, fingerprint=None) -> None:
        """Pin the signed release to the known unedited image before activation."""
        build = content.build_for(self._source_sha)
        if not build or (fingerprint or self.state["game"]["fingerprint"]) != patcher.fingerprint(build["base_sha256"]):
            raise nsr_content.ContentError("Runtime ratings need the stock rollback image. Find Reborn once to rebuild it.")
        content.validate_runtime_image(os.path.join(self.folder, patcher.OUTPUT_NAME), self._source_sha)

    def set_auto_updates(self, on: bool) -> None:
        self.cfg["content_auto"] = bool(on)
        self._save()
        self._set("content", auto=bool(on))
        if on:
            self._spawn(lambda: self.update_content_now(user=False))

    def check_updates(self) -> None:
        """The "Check for updates" button: check and install (explicit request)."""
        self._spawn(lambda: self.update_content_now(user=True))

    def _dolphin_running(self) -> bool:
        p = self._proc
        return p is not None and hasattr(p, "poll") and p.poll() is None

    def _reborn_iso(self) -> str | None:
        iso = self.cfg.get("reborn_iso")
        if iso and os.path.isfile(iso):
            return iso
        return self._patcher.find_reborn_iso()

    def _usable(self, payload: dict) -> bool:
        rel = payload["release"]
        return (rel["min_launcher"] <= nsr_content.LAUNCHER_CONTENT_FORMAT
                and rel["recipe"] == self._patcher.ROLLBACK_RECIPE)

    def _known_content(self):
        """Newest verified content for a rebuild: the cached manifest with its
        packs (downloaded now if missing and online), else None (plain build)."""
        payload = self._content_cache.manifest()
        if not payload or not self._usable(payload):
            return None
        try:
            return self._content_cache.load(payload, self._content_fetch, self.content_base())
        except (OSError, nsr_content.ContentError) as e:
            log(self.folder, f"content for rebuild unavailable: {e}")
            return None

    def _content(self, state: str, text: str, **kw) -> str:
        self._set("content", state=state, text=text, **kw)
        return state

    def _installed_text(self) -> str:
        c = self.state["game"].get("content")
        return f"game content v{c['version']}" if c and c["version"] else "your current game copy"

    def update_content_now(self, user: bool = False, before_play: bool = False) -> str:
        """One update check, installing when allowed (user=True: the button, or
        auto updates on). Returns the resulting content state. Never removes a
        working image: every failure keeps the current nsr-net.iso."""
        with self._lock:
            if self.state["content"]["busy"]:
                return "busy"
            self._set("content", busy=True)
        try:
            return self._update(user, before_play)
        except Exception as e:  # noqa: BLE001  (updates must never break the launcher)
            log(self.folder, f"content update crashed: {e!r}")
            return self._content("error", "Update check failed; keeping your current game copy.")
        finally:
            self._set("content", busy=False)

    def _update(self, user: bool, before_play: bool) -> str:
        base = self.content_base()
        if not base or not nsr_content.trusted_keys(self._content_keys):
            return self._content("off", "Automatic game updates aren't set up in this download.")
        self._content("checking", "Checking for game updates…")
        try:
            raw = self._content_fetch(base + "/manifest", nsr_content.MAX_MANIFEST_BYTES)
            payload = nsr_content.verify_manifest(raw, self._content_keys)
        except OSError as e:
            log(self.folder, f"content check offline: {e}")
            return self._content("offline", f"Couldn't check for updates (offline?). Using {self._installed_text()}.")
        except nsr_content.ContentError as e:
            log(self.folder, f"content manifest rejected: {e}")
            return self._content("error", "An update list was found but couldn't be verified, so it was "
                                          f"ignored. Using {self._installed_text()}.")
        seen = int(self.cfg.get("content_serial", 0) or 0)
        if payload["serial"] < seen:  # an old, validly signed manifest replayed
            log(self.folder, f"content manifest serial {payload['serial']} < {seen}: ignored")
            return self._content("error", "The server sent an older update list than one already seen; "
                                          f"ignored it. Using {self._installed_text()}.")
        if payload["serial"] > seen:
            self.cfg["content_serial"] = payload["serial"]
            self._save()
        self._content_cache.save_manifest(raw)
        self._checked_at = self._clock()
        release = payload["release"]
        what = nsr_content.describe(release)
        self._set("content", latest=release["version"], changelog=release["changelog"])
        installed = self.state["game"].get("content")
        if (installed and installed["version"] == release["version"]
                and bool(self._runtime_content) == self.state["content"]["runtime"]):
            return self._content("current", f"Up to date: {what}")
        if not self._usable(payload):
            return self._content("blocked", f"Game content v{release['version']} needs a newer "
                                            "NFLStreetRollback download. Ask Mason for it.")
        if not user and not self.state["content"]["auto"]:
            return self._content("available", f"Update available: {what}. Click “Check for updates” to install it.")
        if not user and installed is None and self.state["game"]["state"] == "ready":
            # A custom copy (e.g. a hand-built ratings image) is only replaced on request.
            return self._content("available", f"Update available: {what}. Click “Check for updates” to "
                                              "replace your custom game copy with it.")
        if self.state["game"]["state"] not in ("ready",):
            return self._content("available", f"Update available: {what}. Set up your game copy first "
                                              "(Find Reborn automatically); it will include the update.")
        blocker = ("Wait for the current build to finish." if self.state["game"]["busy"] else
                   "Close the game first, then click “Check for updates”." if self._dolphin_running() else
                   "Leave the room first, then click “Check for updates”."
                   if not before_play and (self.state["host"]["state"] != "idle"
                                           or self.state["join"]["state"] != "idle") else "")
        if blocker:
            return self._content("available", f"Update available: {what}. {blocker}")
        iso = self._reborn_iso() if not self.state["content"]["runtime"] else None
        if not iso and not self.state["content"]["runtime"]:
            return self._content("blocked", f"Update available: {what}. Reborn's game file (NSR.iso) is needed for "
                                            "this rebuild. Locate Reborn in Settings → Advanced. Your current game copy is kept.")
        if self._source_sha and self._source_sha not in release["builds"]:
            return self._content("blocked", f"Game content v{release['version']} doesn't support your Reborn "
                                            "version yet. Ask Mason.")
        self._content("downloading", f"Downloading {what}…")
        try:
            content = self._content_cache.load(payload, self._content_fetch, base)
        except OSError as e:
            log(self.folder, f"content download failed: {e}")
            return self._content("offline", f"Couldn't download the update; using {self._installed_text()}. "
                                            "Try “Check for updates” later.")
        except nsr_content.ContentError as e:
            log(self.folder, f"content pack rejected: {e}")
            return self._content("error", "The update's files didn't verify, so nothing was changed.")
        if self.state["content"]["runtime"]:
            try:
                self._validate_runtime(content)
                self._activate_runtime(content)
                self._ready(self.state["game"]["fingerprint"], "Ratings are applied when the game starts.",
                            content.descriptor())
                self.notify(f"Game updated: {what}")
                return self._content("current", f"Up to date: {what}")
            except (OSError, nsr_content.ContentError) as exc:
                self._check_game_now()
                return self._content("blocked", f"Runtime update could not finish: {exc}")
        restore = self._claim_build(f"Updating to game content v{content.version}…")
        if restore is None:
            return self._content("available", f"Update available: {what}. Wait for the current build to finish.")
        self._content("updating", f"Installing {what}…")
        if self._run_build(iso, content, strict=True, restore=restore):
            log(self.folder, f"content: installed v{content.version} (serial {content.serial})")
            self.notify(f"Game updated: {what}")
            return self._content("current", f"Up to date: {what}")
        return self._content("error", f"Couldn't install {what}; your previous game copy was kept.")

    def _preflight_update(self) -> None:
        """Before hosting/joining with auto updates on: catch an update published
        since the last check, so the room doesn't have to reject us."""
        if not self.state["content"]["auto"] or not self.content_base():
            return
        if self._checked_at is not None and self._clock() - self._checked_at < CONTENT_RECHECK_S:
            return
        self.update_content_now(user=False, before_play=True)

    def _content_descriptor(self) -> dict | None:
        return self.state["game"].get("content")

    # -- launching --
    def launch(self, start, open_ui: bool = False) -> bool:
        if not open_ui and self.need_game():
            return False
        try:  # never let a controller-file problem block a match
            self.sync_controllers_now()
        except Exception as e:  # noqa: BLE001
            log(self.folder, f"controller sync failed: {e!r}")
        if start is not None and getattr(start, "telemetry_url", ""):
            # Remember the server and this room's token as registration proof
            # for our own native game-result captures.
            self.uploader.remember_server(start.telemetry_url, start.telemetry_token)
            self._stats_wake.set()
        selected_draft = lobby.draft_settings(start.draft) if start else self.draft_settings()
        if (not open_ui and patcher.read_manifest(self.folder).get("base_family") == patcher.ownimage_build.FAMILY
                and selected_draft != {"order": "alternating", "first": "stock"}):
            self.notify("Retail v1 currently supports Alternating / Stock draft settings only.", "error")
            return
        if (not open_ui and patcher.read_manifest(self.folder).get("base_family") == patcher.ownimage_build.FAMILY
                and (start.quickstart if start else self.state["quickstart"]) != "manual"):
            self.notify("Retail automatic navigation is not qualified yet. Choose Manual navigation.", "error")
            return
        try:
            args, env = dolphin_command(self.folder, start, None, open_ui=open_ui,
                                        name=self.state["name"], fingerprint=self.room_fingerprint(selected_draft),
                                        telemetry=self.state["telemetry"], quickstart=self.state["quickstart"],
                                        icons=self._icons_setting(), resolution=self._resolution_setting())
        except OSError as e:
            self.notify(f"Couldn't prepare the local texture list: {e}. Close the game and retry.", "error")
            return False
        if not open_ui:  # same values the fork logs in nsr-p*.log ("local pad:")
            adapter = env.get("NSR_PAD_ADAPTER")
            log(self.folder, f"controller ({'netplay' if start else 'offline'}): "
                f"kind={env.get('NSR_PAD_KIND', '?')} device={env.get('NSR_PAD_DEVICE') or '-'}"
                + (f" usb-adapter-port={int(adapter) + 1}" if adapter else ""))
            # Which texture folders and button icons this launch shows, so a
            # "MISSING TEXTURES"/wrong-icons report is diagnosable from the log.
            log(self.folder, f"{env.get('NSR_TEXTURES', 'textures ?')} "
                f"resolution={self._resolution_setting() or 'dolphin'}")
            if env.get("NSR_TEXTURES_WARNING"):
                log(self.folder, f"textures warning: {env['NSR_TEXTURES_WARNING']}")
                self.notify(env["NSR_TEXTURES_WARNING"], "error")
        try:
            # The fork truncates this per-player log at connection setup. Clear
            # it here too so a failed launch cannot surface an older mismatch.
            if start is not None and not open_ui:
                with open(env["NSR_LOG"], "w", encoding="utf-8"):
                    pass
            if not open_ui:
                active = self._installed_runtime()
                import draftmodes
                order, first = selected_draft["order"], selected_draft["first"]
                if active:
                    self._validate_runtime(active)
                if active or draftmodes.patch_names(order, first):
                    if draftmodes.patch_names(order, first) and self.state["game"]["fingerprint"] not in {
                            patcher.fingerprint(digest) for _, digest in patcher.KNOWN_INPUTS.values()}:
                        raise nsr_content.ContentError("Runtime draft settings need the stock rollback base. Rebuild your game copy once.")
                    env.update(self._content_cache.runtime_env(active, order, first))
                    args += ["--nsr-runtime-content=1"]
            self._proc = self._popen(args, env=env, cwd=self.folder)
        except (OSError, nsr_content.ContentError) as e:
            self.notify(f"Couldn't start Dolphin ({args[0]}): {e}", "error")
            return False
        if start is not None and not open_ui and hasattr(self._proc, "poll"):
            process = self._proc
            self._spawn(lambda: self._watch_boot_sync(process, env["NSR_LOG"],
                quickstart=start.quickstart != "manual"))
        return True

    def _watch_boot_sync(self, process, path: str, quickstart: bool = False) -> None:
        """Surface the fork's named preboot failure without exposing wire internals.

        Quickstart sessions also wait for the menu handoff and surface a bounded
        navigation failure. Gameplay and later exits belong to Dolphin's own UI.
        """
        deadline = time.monotonic() + (600 if quickstart else 130)  # connect plus bounded navigation
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
                if "quickstart FAILED" in line:
                    self.notify("Quickstart could not finish the menus. Reopen the room with "
                                "Manual (menus), or use the latest download.", "error")
                    return
                if "quickstart COMPLETE" in line or ("boot sync PASSED" in line and not quickstart):
                    return
            if process.poll() is not None:
                return
            time.sleep(0.2)

    def set_quickstart(self, selected: str) -> None:
        """Freeze a room's game mode at creation so its lobby/start cannot drift."""
        if self.state["host"]["state"] != "idle":
            return
        try:
            selected = lobby.quickstart_id(selected)
        except ConnectionError as e:
            self.notify(str(e), "error")
            return
        self.cfg["quickstart"] = selected
        self._save()
        self._set(quickstart=selected)

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

        def work():
            self._preflight_update()
            h = self._lobby.RoomHost(self.rv_server, self.state["name"], self.state["game"]["fingerprint"],
                                     on_change=self._roster, on_error=lambda why: self._set("host", info=why),
                                     content=self._content_descriptor(), quickstart=self.state["quickstart"],
                                     draft=self.draft_settings())
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
        self._set("host", state="opening", mode="ip", info="Opening a direct lobby…")
        try:
            h = self._lobby.LobbyHost(self.state["name"], self.state["game"]["fingerprint"],
                                      on_change=self._roster,
                                      on_reject=lambda who, why: self._set("host", info=f"Refused {who}: {why}"),
                                      content=self._content_descriptor(), quickstart=self.state["quickstart"],
                                      draft=self.draft_settings())
            h.open()
        except OSError as e:
            self._set("host", state="idle", mode="")
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
            self.cfg[DELAY_CONFIG_KEY] = delay
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
        self._set("join", state="waiting", players=[], quickstart="manual", draft_label="",
                  info=f"Joining {target}… waiting for the host to start.")
        on_lobby = lambda names: self._set("join", players=list(names))  # noqa: E731

        def attempt():
            kw = {"on_lobby": on_lobby, "cancel": cancel, "content": self._content_descriptor(),
                  "on_game_mode": lambda selected: self._set("join", quickstart=selected),
                  "on_draft": lambda selected: self._set("join", draft_label=lobby.draft_label(selected))}
            if direct:
                return self._lobby.join(target, self.state["name"], self.state["game"]["fingerprint"], **kw)
            return self._lobby.join_room(self.rv_server, target, self.state["name"],
                                         self.state["game"]["fingerprint"], **kw)

        def work():
            self._preflight_update()
            start = None
            for retry in (False, True):
                try:
                    start = attempt()
                    break
                except ConnectionError as e:
                    # The room says our game content is older: with auto updates on,
                    # update now and try once more; else say what to click.
                    if getattr(e, "action", "") != nsr_content.UPDATE_ACTION or retry:
                        self._set("join", state="idle", info=str(e))
                        return
                    if not self.state["content"]["auto"]:
                        self._set("join", state="idle", info=str(e))
                        return
                    self._set("join", info="Your game content is older than the room's. Updating first…")
                    if self.update_content_now(user=True, before_play=True) != "current":
                        self._set("join", state="idle",
                                  info=f"{e}\n{self.state['content']['text']}")
                        return
                    self._set("join", info=f"Updated. Joining {target} again…")
            if start is None:
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
        # The self-contained launcher discovers only its own native captures.
        # Reborn assets, manifests and source paths never enter the uploader.
        own_stats = os.path.join(self.user_dir, "GameStats")
        dirs = [own_stats] if os.path.isdir(own_stats) else []
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
            text = "No local game results folder found yet. " + text
        self._set("game_stats", state=res["state"], text=text, sent=res["total_sent"], pending=res["pending"])
        if res.get("sent"):
            log(self.folder, f"game stats: sent {res['sent']}, {res['pending']} pending")
        return res

    def _stats_loop(self) -> None:
        while not self._stop.is_set():
            self.upload_stats_now()
            self._stats_wake.wait(gamestats.INTERVAL_S)
            self._stats_wake.clear()

    # -- local presentation import (serialized by the existing setup busy flag) --
    def _ensure_assets(self, force: bool = False, install: str | None = None) -> None:
        """Metadata-check or import inside setup; normal composition is local only."""
        if patcher.read_manifest(self.folder).get("base_family") == patcher.ownimage_build.FAMILY:
            return  # Native retail has no Reborn presentation import dependency.
        old = textures.read_store(self.folder)
        install = install or old.get("install") or self.reborn_install(search=True)
        if not install and not old and not force:
            textures.write_list(self.user_dir, self.refresh_textures())  # clear legacy source references
            return
        label = self._patcher.read_manifest(self.folder).get("source_label") if hasattr(self._patcher, "read_manifest") else ""
        manifest = textures.ensure_store(self.folder, install, self._env, force=force,
            version=(label or "unknown").removeprefix("Reborn "), progress=lambda d, t: self._set("game",
                text="Importing Reborn textures into your local folder…", progress=round(d * 100 / max(t, 1))))
        # Publish the local list during setup too: the developer batch entry
        # can then launch immediately without a first normal gameplay launch.
        textures.write_list(self.user_dir, self.refresh_textures())
        if not self._textures_logged:
            log(self.folder, textures.store_summary(manifest))
            self._textures_logged = True

    def _import_assets_setup(self, force: bool = False) -> None:
        """Keep a working generation on failure; surface first-import disk errors."""
        with self._lock:
            if (self._dolphin_running() or self.state["content"]["busy"]
                    or self.state["host"]["state"] != "idle" or self.state["join"]["state"] != "idle"):
                self.notify("Close the game, leave your room and wait for updates before importing.", "error")
                return
            before = self._claim_build("Checking your local texture store…")
            if before is None:
                return
        try:
            self._ensure_assets(force)
            if before["state"] == "ready":
                self._set("game", **{**before, "busy": True, "progress": None})
            else:
                self._check_game_now()  # retry after a first-import failure
            if force:
                self.notify("Reborn textures imported into your local folder.")
        except (OSError, ValueError) as e:
            if textures.read_store(self.folder):
                self._set("game", **{**before, "busy": True, "progress": None})
                self.notify(f"{e} Your existing local textures are kept.", "error")
                if not self._textures_logged:
                    log(self.folder, textures.store_summary(textures.read_store(self.folder)))
                    self._textures_logged = True
            else:
                self._set("game", state="error", fingerprint="", text=str(e), progress=None)
            log(self.folder, f"local texture import: {e}")
        finally:
            self._set("game", busy=False)

    def reimport_reborn(self) -> None:
        """Settings → Advanced: refresh assets, preserving our ISO and controls."""
        self._spawn(lambda: self._import_assets_setup(force=True))

    def _startup_checks(self) -> None:
        self._check_game_now()
        if self.state["game"]["state"] == "ready":
            self._import_assets_setup()
        self.update_content_now(user=False)  # auto: install; manual: just report
        # One-click start: a missing or outdated game copy is built right away
        # from the Reborn install we can find (what "Find Reborn automatically"
        # does), so players never need to run the separate patcher first. If
        # Reborn can't be found, the Game card still asks them to browse.
        game = self.state["game"]
        if game.get("state") in ("missing", "stale") and not game.get("busy"):
            log(self.folder, f"startup: game copy {game.get('state')}; building it automatically")
            self.find_game()

    def startup(self) -> None:
        def setup():
            self.sync_controllers_now(search=True)
            self._startup_checks()
        self._spawn(setup)
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
    def set_auto_updates(self, on): self._l.set_auto_updates(bool(on))
    def set_content_runtime(self, on): self._l.set_content_runtime(bool(on))
    def check_updates(self): self._l.check_updates()
    def set_delay(self, delay): self._l.set_delay(delay)
    def set_quickstart(self, selected): self._l.set_quickstart(selected)
    def set_button_icons(self, choice): self._l.set_button_icons(str(choice))
    def set_resolution(self, value): self._l.set_resolution(value)
    # Both manual and signed-content rebuilds preserve these image choices.
    def set_draft(self, order, first): self._l.set_draft(order, first)
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
    def reimport_reborn(self): self._l.reimport_reborn()
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
    delay_names = ["Auto"] + [str(d) for d in range(1, MAX_DELAY + 1)]
    delay_text = tk.StringVar(value=delay_names[launcher.state["delay"]])

    class _DelayVar:  # start_game(delay_var.get()): 0 = Auto, else frames
        @staticmethod
        def get() -> int:
            return delay_names.index(delay_text.get()) if delay_text.get() in delay_names else AUTO_DELAY
    delay_var = _DelayVar()
    ttk.Label(hp, text="Input delay for everyone (Auto picks 1-4 frames from the connection):",
              style="Muted.TLabel").grid(row=4, column=0, columnspan=3, sticky="w", pady=(10, 0))
    delay_box = ttk.Combobox(hp, values=delay_names, textvariable=delay_text, width=6, state="readonly")
    delay_box.bind("<<ComboboxSelected>>", lambda _e: launcher.set_delay(delay_var.get()))
    delay_box.grid(row=4, column=3, sticky="w", pady=(10, 0))
    ttk.Button(hp, text="Practice offline", command=launcher.play_offline).grid(row=5, column=0, sticky="w", pady=8)

    # Native fallback exposes the same selection; a created room locks it.
    ttk.Label(hp, text="Game mode", style="Panel.TLabel").grid(row=6, column=0, sticky="w")
    mode_var = tk.StringVar(value=lobby.QUICKSTARTS[launcher.state["quickstart"]])
    mode_picker = ttk.Combobox(hp, textvariable=mode_var, state="readonly",
                              values=list(lobby.QUICKSTARTS.values()), width=24)
    mode_picker.grid(row=6, column=1, columnspan=3, sticky="w")
    mode_picker.bind("<<ComboboxSelected>>", lambda e: launcher.set_quickstart(
        next(k for k, v in lobby.QUICKSTARTS.items() if v == mode_var.get())))
    ttk.Label(hp, text="Quick Play or Pickup Game goes straight to Select Controller (pick sides, first pick / ball first).\n"
                       "Practice uses this choice too. Manual keeps offline saves.",
              style="Muted.TLabel", wraplength=650).grid(row=7, column=0, columnspan=4, sticky="w", pady=6)

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
    ttk.Button(sp, text="Re-import from Reborn", command=launcher.reimport_reborn).grid(row=8, column=2, sticky="w", pady=8)
    b_find = ttk.Button(sp, text="Find Reborn automatically", command=launcher.find_game)
    b_browse = ttk.Button(sp, text="Browse for NSR.iso…")
    b_find.grid(row=8, column=0, sticky="w")
    b_browse.grid(row=8, column=1, sticky="w", padx=6)
    content_text = label(sp, 9, pady=(14, 4))
    b_updates = ttk.Button(sp, text="Check for updates", command=launcher.check_updates)
    b_updates.grid(row=10, column=0, sticky="w")
    auto_var = tk.BooleanVar(value=launcher.state["content"]["auto"])
    ttk.Checkbutton(sp, text="Install game updates automatically (roster/rating edits from Mason)",
                    variable=auto_var, command=lambda: launcher.set_auto_updates(auto_var.get())) \
        .grid(row=11, column=0, columnspan=4, sticky="w", pady=(6, 0))

    runtime_var = tk.BooleanVar(value=launcher.state["content"]["runtime"])
    ttk.Checkbutton(sp, text="Apply ratings at game start (off: rebuild the image)",
                    variable=runtime_var, command=lambda: launcher.set_content_runtime(runtime_var.get())) \
        .grid(row=12, column=0, columnspan=4, sticky="w", pady=(6, 0))
    ttk.Label(hp, text="Pickup draft / first team").grid(row=8, column=0, sticky="w", pady=(12, 0))
    draft_order = tk.StringVar(value=launcher.state["game"]["draft_order"])
    draft_first = tk.StringVar(value=launcher.state["game"]["draft_first"])
    order_box = ttk.Combobox(hp, textvariable=draft_order, values=("alternating", "snake", "cocky"), state="readonly", width=14)
    first_box = ttk.Combobox(hp, textvariable=draft_first, values=("stock", "team-one", "team-two"), state="readonly", width=14)
    order_box.grid(row=9, column=0, sticky="w")
    first_box.grid(row=9, column=1, sticky="w")
    # Both native sides may contain the host; these choices deliberately name teams.
    order_box.bind("<<ComboboxSelected>>", lambda _: launcher.set_draft(draft_order.get(), draft_first.get()))
    first_box.bind("<<ComboboxSelected>>", lambda _: launcher.set_draft(draft_order.get(), draft_first.get()))
    ttk.Label(hp, text="Cocky: seven picks each. Team One/Two are in-game sides. Shared with joiners; applied at Select Controller and the draft.",
              wraplength=560).grid(row=10, column=0, columnspan=4, sticky="w")

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
            host_info.configure(text=h["info"] + "\n" + lobby.draft_label(launcher.draft_settings()))
            mode_var.set(lobby.QUICKSTARTS[s["quickstart"]])
            mode_picker.configure(state="readonly" if h["state"] == "idle" else "disabled")
            set_list(host_roster, h["players"])
            idle = h["state"] == "idle"
            enable(b_open, idle and s["rooms"]); enable(b_ip, idle)
            enable(b_close, not idle); enable(b_start, h["state"] == "open" and h["can_start"])
            join_info.configure(text=j["info"] + "\nGame mode: " + lobby.QUICKSTARTS[j["quickstart"]] + "\n" + j.get("draft_label", ""))
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
            setup_idle = (not g["busy"] and not s["content"]["busy"]
                          and h["state"] == "idle" and j["state"] == "idle")
            enable(b_find, setup_idle); enable(b_browse, setup_idle)
            draft_order.set(g["draft_order"]); draft_first.set(g["draft_first"])
            order_box.configure(state="readonly" if setup_idle else "disabled")
            first_box.configure(state="readonly" if setup_idle else "disabled")
            ct = s["content"]
            content_text.configure(text=(ct["text"] or "No game updates installed.")
                                   + (f"\nv{ct['latest']}: {ct['changelog']}" if ct["latest"] and ct["changelog"]
                                      and ct["state"] != "off" else ""))
            enable(b_updates, not ct["busy"] and not g["busy"])
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
