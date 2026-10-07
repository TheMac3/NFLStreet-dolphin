#!/usr/bin/env python3
"""nsr_launcher.py: "NSR Netplay", the friend-facing launcher for NFLStreet netplay.

A small window that replaces command lines and .bat files. It lives in the
NFLStreet-dolphin folder (next to Dolphin.exe) and does three things:

  1. Game: find NFL Street Reborn's NSR.iso (or browse to it), build the
     netplay image (nsr_patcher.prepare_image), and show its fingerprint.
  2. Play: Host (Create room: shows a code like NSR-4K7QX, lists who joined,
     Start) or Join (type the code, wait for Start), via the rendezvous server
     (room codes + hole punching, so usually no router setup). "Host by IP
     address" / typing an IP is the advanced fallback. Then it starts the
     netplay Dolphin with every setting filled in.
  3. Practice offline, and "Set up controllers" (opens Dolphin's own window).

Reborn is never modified: its files are only read, this Dolphin runs in
portable mode with its own settings and saves (User/ next to it), and
Reborn's HD textures are loaded read-only from its install.

Run from source:  python3 tools/nsr_launcher.py   (needs tkinter)
Dev overrides:    NSR_APP_DIR (folder with Dolphin + nsr-net.iso),
                  NSR_DOLPHIN (Dolphin executable), NSR_DOLPHIN_ARGS (extra args)
"""
from __future__ import annotations

import json
import os
import queue
import secrets
import shlex
import socket
import subprocess
import sys
import threading
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import isopatch  # noqa: E402
import nsr_lobby as lobby  # noqa: E402
import nsr_patcher as patcher  # noqa: E402

APP_TITLE = "NSR Netplay"
CONFIG_NAME = "nsr-launcher.json"
DEFAULT_DELAY = 2


# --- pure helpers (tested without a GUI) --------------------------------------
def app_dir() -> str:
    if os.environ.get("NSR_APP_DIR"):
        return os.environ["NSR_APP_DIR"]
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.getcwd()


def dolphin_command(folder: str, start: lobby.StartInfo | None, reborn_install: str | None,
                    open_ui: bool = False) -> tuple[list[str], dict[str, str]]:
    """Command line and environment for the netplay Dolphin.

    start=None: practice offline. open_ui=True: just open Dolphin's window
    (controller setup), no game. Every setting that affects determinism is
    passed explicitly so all machines match regardless of local Dolphin config.
    """
    exe = os.environ.get("NSR_DOLPHIN") or os.path.join(folder, "Dolphin.exe")
    args = [exe, "-u", os.path.join(folder, "User")]
    args += shlex.split(os.environ.get("NSR_DOLPHIN_ARGS", ""))
    env = {k: v for k, v in os.environ.items() if not k.startswith("NSR_")}
    if open_ui:
        return args, env
    players = start.players if start else 1
    if reborn_install:
        args += ["-C", f"Dolphin.General.LoadPath={os.path.join(reborn_install, 'content')}",
                 "-C", "GFX.Settings.HiresTextures=True"]
    args += ["-C", "Dolphin.Core.CPUThread=False",
             "-C", "Dolphin.Core.EnableCustomRTC=True",
             "-C", "Dolphin.Core.CustomRTCValue=0x30000000"]
    for port in range(4):  # controllers in ports 1..players; the rest empty
        args += ["-C", f"Dolphin.Core.SIDevice{port}={6 if port < players else 0}"]
    # Per-match RNG seed: agreed in the lobby for netplay; fresh per launch
    # offline, so Quick Play differs every time either way.
    env["NSR_SEED"] = str(start.seed if start else secrets.randbits(32))
    if start:
        env.update({
            "NSR_NETPLAY": "1",
            "NSR_PLAYER": str(start.player),
            "NSR_PLAYERS": str(start.players),
            "NSR_PORT": str(lobby.PORT),
            "NSR_TOPOLOGY": "star",
            "NSR_DELAY": str(start.delay),
            "NSR_LOG": os.path.join(folder, f"nsr-p{start.player}.log"),
        })
        if start.rendezvous:
            # Room code: hole punching through the rendezvous server.
            env.update({"NSR_RENDEZVOUS": start.rendezvous, "NSR_ROOM": start.room,
                        "NSR_KEY": start.key, "NSR_LAN_IP": lan_address()})
            env.pop("NSR_PORT", None)
        elif start.player > 0:
            env["NSR_PEERS"] = f"{start.host_address}:{lobby.PORT}"
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


# --- GUI ------------------------------------------------------------------------
def run_gui(selftest: bool = False) -> int:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    folder = app_dir()
    cfg = load_config(folder)
    events: queue.Queue = queue.Queue()  # (callable,) from worker threads
    state = {"fingerprint": None, "host": None, "join_cancel": None, "busy": False}

    root = tk.Tk()
    root.title(APP_TITLE)
    root.minsize(560, 520)
    bg, panel, fg, accent, muted = "#141419", "#1e1e26", "#f2f2f2", "#d7263d", "#9a9aa8"
    root.configure(bg=bg)
    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure(".", background=bg, foreground=fg, fieldbackground=panel, font=("Segoe UI", 10))
    style.configure("Panel.TFrame", background=panel)
    style.configure("Panel.TLabel", background=panel, foreground=fg)
    style.configure("Muted.TLabel", background=panel, foreground=muted)
    style.configure("Title.TLabel", background=bg, foreground=fg, font=("Segoe UI", 18, "bold"))
    style.configure("Sub.TLabel", background=bg, foreground=muted)
    style.configure("Big.TLabel", background=panel, foreground=fg, font=("Segoe UI", 12, "bold"))
    style.configure("Accent.TButton", background=accent, foreground="white", padding=8,
                    font=("Segoe UI", 10, "bold"))
    style.map("Accent.TButton", background=[("active", "#b81f33"), ("disabled", "#5a2a31")])
    style.configure("TButton", background=panel, foreground=fg, padding=6)
    style.configure("TNotebook", background=bg, borderwidth=0)
    style.configure("TNotebook.Tab", background=panel, foreground=fg, padding=(14, 6))
    style.map("TNotebook.Tab", background=[("selected", accent)])
    style.configure("Horizontal.TProgressbar", background=accent, troughcolor=panel)

    def ui(fn):  # run fn on the Tk thread
        events.put(fn)

    def pump():
        try:
            while True:
                events.get_nowait()()
        except queue.Empty:
            pass
        root.after(50, pump)

    outer = ttk.Frame(root, padding=16)
    outer.pack(fill="both", expand=True)
    ttk.Label(outer, text="NFL Street Netplay", style="Title.TLabel").pack(anchor="w")
    ttk.Label(outer, text="Play NFL Street Reborn online with friends (2–4 players). "
                          "Reborn itself is never changed.", style="Sub.TLabel").pack(anchor="w", pady=(0, 10))

    # 1. Game
    game = ttk.Frame(outer, style="Panel.TFrame", padding=12)
    game.pack(fill="x")
    ttk.Label(game, text="1. Game", style="Big.TLabel").grid(row=0, column=0, sticky="w")
    game_status = ttk.Label(game, text="Checking…", style="Panel.TLabel", wraplength=500)
    game_status.grid(row=1, column=0, columnspan=3, sticky="w", pady=(4, 6))
    progress = ttk.Progressbar(game, length=300, mode="determinate")
    btn_find = ttk.Button(game, text="Find Reborn automatically")
    btn_browse = ttk.Button(game, text="Browse for NSR.iso…")
    btn_find.grid(row=3, column=0, sticky="w")
    btn_browse.grid(row=3, column=1, sticky="w", padx=6)

    # 2. You + play
    you = ttk.Frame(outer, style="Panel.TFrame", padding=12)
    you.pack(fill="x", pady=10)
    ttk.Label(you, text="2. Play", style="Big.TLabel").grid(row=0, column=0, sticky="w")
    ttk.Label(you, text="Your name", style="Panel.TLabel").grid(row=1, column=0, sticky="w", pady=4)
    name_var = tk.StringVar(value=cfg.get("name", os.environ.get("USERNAME", "Player"))[:24])
    ttk.Entry(you, textvariable=name_var, width=24).grid(row=1, column=1, sticky="w")
    ttk.Label(you, text="Your controller is always Dolphin's Port 1.", style="Muted.TLabel") \
        .grid(row=2, column=0, columnspan=2, sticky="w")
    ttk.Button(you, text="Set up controllers", command=lambda: launch(None, open_ui=True)) \
        .grid(row=1, column=2, padx=8)

    tabs = ttk.Notebook(outer)
    tabs.pack(fill="both", expand=True)
    host_tab = ttk.Frame(tabs, style="Panel.TFrame", padding=12)
    join_tab = ttk.Frame(tabs, style="Panel.TFrame", padding=12)
    solo_tab = ttk.Frame(tabs, style="Panel.TFrame", padding=12)
    tabs.add(host_tab, text="Host")
    tabs.add(join_tab, text="Join")
    tabs.add(solo_tab, text="Practice offline")

    # Host tab
    rv_server = rendezvous_server(folder)
    host_info = ttk.Label(host_tab, text=(
        "Create a room and give your friends the code. No router setup needed."
        if rv_server else "Room codes aren't set up in this download (no room server). "
                          "Use “Host by IP address” below."),
        style="Panel.TLabel", wraplength=500, justify="left")
    host_info.grid(row=0, column=0, columnspan=4, sticky="w")
    code_var = tk.StringVar(value="")
    code_label = ttk.Label(host_tab, textvariable=code_var, style="Title.TLabel")
    code_label.configure(background=panel)
    code_label.grid(row=1, column=0, columnspan=2, sticky="w", pady=(6, 0))
    btn_copy = ttk.Button(host_tab, text="Copy code",
                          command=lambda: (root.clipboard_clear(), root.clipboard_append(code_var.get())))
    roster_host = tk.Listbox(host_tab, height=4, bg=bg, fg=fg, highlightthickness=0, borderwidth=0)
    roster_host.grid(row=2, column=0, columnspan=4, sticky="we", pady=8)
    btn_open = ttk.Button(host_tab, text="Create room", style="Accent.TButton",
                          state="normal" if rv_server else "disabled")
    btn_start = ttk.Button(host_tab, text="Start game", style="Accent.TButton", state="disabled")
    btn_close = ttk.Button(host_tab, text="Close", state="disabled")
    btn_open_ip = ttk.Button(host_tab, text="Host by IP address (advanced)")
    btn_open.grid(row=3, column=0, sticky="w")
    btn_start.grid(row=3, column=1, sticky="w", padx=6)
    btn_close.grid(row=3, column=2, sticky="w")
    btn_open_ip.grid(row=3, column=3, sticky="e", padx=(12, 0))
    delay_var = tk.IntVar(value=int(cfg.get("delay", DEFAULT_DELAY)))
    ttk.Label(host_tab, text="Input delay (frames, raise if laggy):", style="Muted.TLabel") \
        .grid(row=4, column=0, sticky="w", pady=(10, 0))
    ttk.Spinbox(host_tab, from_=0, to=8, textvariable=delay_var, width=4) \
        .grid(row=4, column=1, sticky="w", pady=(10, 0))

    # Join tab
    ttk.Label(join_tab, text="Room code (or host IP)", style="Panel.TLabel").grid(row=0, column=0, sticky="w")
    addr_var = tk.StringVar(value=cfg.get("last_host", ""))
    ttk.Entry(join_tab, textvariable=addr_var, width=30).grid(row=0, column=1, sticky="w", padx=6)
    join_info = ttk.Label(join_tab, text="", style="Panel.TLabel", wraplength=500, justify="left")
    join_info.grid(row=1, column=0, columnspan=3, sticky="w", pady=6)
    roster_join = tk.Listbox(join_tab, height=4, bg=bg, fg=fg, highlightthickness=0, borderwidth=0)
    roster_join.grid(row=2, column=0, columnspan=3, sticky="we")
    btn_join = ttk.Button(join_tab, text="Join", style="Accent.TButton")
    btn_leave = ttk.Button(join_tab, text="Leave", state="disabled")
    btn_join.grid(row=3, column=0, sticky="w", pady=8)
    btn_leave.grid(row=3, column=1, sticky="w")

    # Practice tab
    ttk.Label(solo_tab, text="Play on this PC only (same setup as netplay, no network).",
              style="Panel.TLabel").pack(anchor="w")
    btn_solo = ttk.Button(solo_tab, text="Play offline", style="Accent.TButton",
                          command=lambda: launch(None))
    btn_solo.pack(anchor="w", pady=8)

    def set_roster(box, names):
        box.delete(0, "end")
        for i, n in enumerate(names):
            box.insert("end", f"  Player {i + 1}: {n}" + ("  (host)" if i == 0 else ""))

    def need_game() -> bool:
        if not state["fingerprint"]:
            messagebox.showinfo(APP_TITLE, "Set up the game first (step 1).")
            return True
        return False

    # --- game image -------------------------------------------------------
    def show_ready(fp, note=""):
        state["fingerprint"] = fp
        game_status.configure(text=f"✓ Ready. Game fingerprint {fp}{note}\n"
                                   "Everyone you play with must have the same fingerprint.")
        progress.grid_forget()

    def prepare(iso):
        if state["busy"]:
            return
        state["busy"] = True
        btn_find.state(["disabled"]); btn_browse.state(["disabled"])
        progress.grid(row=2, column=0, columnspan=3, sticky="w", pady=4)
        labels = {"check": "Checking Reborn's game file…", "build": "Building the netplay copy…",
                  "verify": "Fingerprinting…"}

        def work():
            try:
                r = patcher.prepare_image(iso, folder, progress=lambda st, d, t: ui(
                    lambda: (game_status.configure(text=labels[st]),
                             progress.configure(value=d * 100 / max(t, 1)))))
                note = "" if r.matches_reference else (
                    "\n(Unrecognized Reborn version: compare fingerprints with friends.)"
                    if r.matches_reference is None else
                    "\nWARNING: doesn't match the reference build; ask Mason before playing.")
                cfg["reborn_iso"] = iso
                cfg["reborn_install"] = r.reborn_install
                save_config(folder, cfg)
                ui(lambda: show_ready(r.fingerprint, note))
            except isopatch.PatchError:
                ui(lambda: game_status.configure(
                    text="This Reborn version isn't supported yet (the game code changed). "
                         "Nothing was written. Ask Mason for an updated launcher."))
            except OSError as e:
                ui(lambda e=e: game_status.configure(text=f"Couldn't build the game copy: {e}"))
            finally:
                state["busy"] = False
                ui(lambda: (btn_find.state(["!disabled"]), btn_browse.state(["!disabled"])))

        threading.Thread(target=work, daemon=True).start()

    def find_auto():
        game_status.configure(text="Looking for NFL Street Reborn…")

        def work():
            iso = patcher.find_reborn_iso()
            ui(lambda: prepare(iso) if iso else game_status.configure(
                text="Couldn't find Reborn automatically. Click “Browse for NSR.iso…” and pick "
                     "the file in your Reborn folder: content\\games\\NSR.iso"))

        threading.Thread(target=work, daemon=True).start()

    def browse():
        iso = filedialog.askopenfilename(title="Pick NFL Street Reborn's NSR.iso (content\\games)",
                                         filetypes=[("Reborn game image", "NSR.iso"), ("ISO", "*.iso")])
        if iso:
            prepare(iso)

    btn_find.configure(command=find_auto)
    btn_browse.configure(command=browse)

    def check_existing():
        def work():
            fp = patcher.image_fingerprint(folder)
            ui(lambda: show_ready(fp) if fp else game_status.configure(
                text="Not set up yet. Click “Find Reborn automatically”."))
        threading.Thread(target=work, daemon=True).start()

    # --- launching -----------------------------------------------------------
    def launch(start, open_ui=False):
        if not open_ui and need_game():
            return
        args, env = dolphin_command(folder, start, cfg.get("reborn_install"), open_ui=open_ui)
        try:
            subprocess.Popen(args, env=env, cwd=folder)
        except OSError as e:
            messagebox.showerror(APP_TITLE, f"Couldn't start Dolphin ({args[0]}): {e}")

    # --- host -----------------------------------------------------------------
    def roster_changed(names):
        ui(lambda: (set_roster(roster_host, names),
                    btn_start.state(["!disabled"] if len(names) > 1 else ["disabled"])))

    def hosting_ui(active: bool):
        for b in (btn_open, btn_open_ip):
            b.state(["disabled"] if active or (b is btn_open and not rv_server) else ["!disabled"])
        btn_close.state(["!disabled"] if active else ["disabled"])
        if not active:
            btn_start.state(["disabled"])
            code_var.set("")
            btn_copy.grid_forget()
            set_roster(roster_host, [])

    def host_room():
        if need_game():
            return
        cfg["name"] = name_var.get().strip() or "Host"
        save_config(folder, cfg)
        hosting_ui(True)
        host_info.configure(text="Creating a room…")
        h = lobby.RoomHost(rv_server, cfg["name"], state["fingerprint"], on_change=roster_changed,
                           on_error=lambda why: ui(lambda: host_info.configure(text=why)))

        def work():
            try:
                code = h.open()
            except ConnectionError as e:
                ui(lambda e=e: (host_info.configure(text=f"{e}\nYou can still “Host by IP address”."),
                                hosting_ui(False)))
                return
            state["host"] = h
            ui(lambda: (code_var.set(code), btn_copy.grid(row=1, column=2, sticky="w"),
                        host_info.configure(text="Room open. Give your friends this code; "
                                                 "click Start when everyone's listed.")))

        threading.Thread(target=work, daemon=True).start()

    def host_ip():
        if need_game():
            return
        cfg["name"] = name_var.get().strip() or "Host"
        save_config(folder, cfg)
        try:
            h = lobby.LobbyHost(cfg["name"], state["fingerprint"], on_change=roster_changed,
                                on_reject=lambda who, why: ui(lambda: host_info.configure(
                                    text=f"Refused {who}: {why}")))
            h.open()
        except OSError as e:
            messagebox.showerror(APP_TITLE, f"Couldn't open the lobby on port {lobby.PORT}: {e}")
            return
        state["host"] = h
        hosting_ui(True)
        lan = lan_address()
        host_info.configure(text=f"Lobby open. Same network? Friends join with:  {lan}\n"
                                 "Finding your internet address…")

        def find_public():
            pub = public_address()
            ui(lambda: host_info.configure(text=(
                f"Lobby open (by IP). Friends join with your address:\n"
                f"   Internet:  {pub}      Same network:  {lan}\n"
                f"For internet play, forward TCP and UDP port {lobby.PORT} on your router to this PC.")
                if pub else f"Lobby open (by IP). Same network? Friends join with:  {lan}\n"
                            f"(Couldn't look up your internet address.)"))

        threading.Thread(target=find_public, daemon=True).start()

    def host_start():
        h = state["host"]
        if not h:
            return
        btn_start.state(["disabled"])

        def work():
            try:
                me = h.start(delay=delay_var.get())
            except (ValueError, ConnectionError) as e:
                ui(lambda e=e: (messagebox.showinfo(APP_TITLE, str(e)), btn_start.state(["!disabled"])))
                return
            cfg["delay"] = delay_var.get()
            save_config(folder, cfg)
            state["host"] = None
            ui(lambda: (hosting_ui(False),
                        host_info.configure(text=f"Started a {me.players}-player game. Have fun!"),
                        launch(me)))

        threading.Thread(target=work, daemon=True).start()

    def host_close():
        if state["host"]:
            state["host"].close()
            state["host"] = None
        hosting_ui(False)
        host_info.configure(text="Closed.")

    btn_open.configure(command=host_room)
    btn_open_ip.configure(command=host_ip)
    btn_start.configure(command=host_start)
    btn_close.configure(command=host_close)

    # --- join -----------------------------------------------------------------
    def do_join():
        if need_game():
            return
        target = addr_var.get().strip()
        if not target:
            join_info.configure(text="Type the room code from the host.")
            return
        direct = looks_like_ip(target)
        if not direct and not rv_server:
            join_info.configure(text="Room codes aren't set up in this download. Ask the host for "
                                     "their IP address instead.")
            return
        cfg["name"] = name_var.get().strip() or "Player"
        cfg["last_host"] = target
        save_config(folder, cfg)
        cancel = threading.Event()
        state["join_cancel"] = cancel
        btn_join.state(["disabled"]); btn_leave.state(["!disabled"])
        join_info.configure(text=f"Joining {target}… waiting for the host to start.")
        on_lobby = lambda names: ui(lambda: set_roster(roster_join, names))  # noqa: E731

        def work():
            try:
                if direct:
                    start = lobby.join(target, cfg["name"], state["fingerprint"],
                                       on_lobby=on_lobby, cancel=cancel)
                else:
                    start = lobby.join_room(rv_server, target, cfg["name"], state["fingerprint"],
                                            on_lobby=on_lobby, cancel=cancel)
                ui(lambda: (join_info.configure(
                    text=f"Starting! You're player {start.player + 1} of {start.players}."),
                    launch(start)))
            except ConnectionError as e:
                ui(lambda e=e: join_info.configure(text=str(e)))
            finally:
                ui(lambda: (btn_join.state(["!disabled"]), btn_leave.state(["disabled"])))

        threading.Thread(target=work, daemon=True).start()

    def do_leave():
        if state["join_cancel"]:
            state["join_cancel"].set()

    btn_join.configure(command=do_join)
    btn_leave.configure(command=do_leave)

    def on_close():
        if state["host"]:
            state["host"].close()
        do_leave()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    pump()
    check_existing()
    if selftest:
        root.update()
        root.after(300, root.destroy)
    root.mainloop()
    return 0


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    return run_gui(selftest="--selftest" in argv)


if __name__ == "__main__":
    sys.exit(main())
