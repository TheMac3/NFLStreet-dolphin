#!/usr/bin/env python3
"""nsr_lobby.py: host/join lobby for NFLStreet netplay (2-4 players).

Two ways to meet:
  - Room codes (default): through the rendezvous server
    (services/rendezvous/nsr_rendezvous.py). The host gets a code like
    NSRB-4K7QX, friends type it in, and Dolphin hole-punches the game traffic,
    so usually nobody forwards ports. See RoomHost / join_room below.
  - Direct (advanced fallback): the host's PC runs the lobby itself and
    friends join by IP address; the host forwards TCP+UDP 51515. LobbyHost /
    join below.

The launcher's "Host" and "Join" buttons use this. It only assigns player
numbers and agrees on settings; the game traffic itself is Dolphin's
(NSRNetplay, UDP, star topology: everyone talks to the host). The host's
router needs TCP *and* UDP port 51515 forwarded for internet play.

Protocol: one TCP connection per joining player to the host, newline-delimited
JSON messages.

  joiner -> host  {"type": "hello", "name": str, "fingerprint": str, "version": int,
                   "mode": "rollback"|"lockstep", "content": {"version", "serial"}}
  host -> joiner  {"type": "reject", "reason": str, "action": str}  (then closes;
                                                                    action optional)
  host -> all     {"type": "lobby", "players": [name, ...]}          (on any change;
                                                                    index = player number)
  host -> joiner  {"type": "start", "player": k, "players": n, "delay": d, "seed": u32,
                   "mode": str}
  host -> joiner  {"type": "closed"}                                (host cancelled)

The host is always player 0. Players keep their join order. A joiner whose
game image fingerprint differs from the host's is refused before anything
starts, since netplay needs byte-identical images.

Netplay mode. The patched image decides what a machine can play: the
rollback image (nsr_patcher, this release) lacks the milestone-1 lockstep
hook and vice versa. So the mode is not a host choice. Every request carries
the launcher's mode, the host/room refuses a different one with an "update"
message, and a joiner refuses a start whose mode differs from its own. The
fingerprint check catches mixed images on its own as well (different recipe,
different bytes), which is what keeps old room servers that ignore "mode"
safe (see RENDEZVOUS_VERSION below).

Content updates (docs/content-updates.md). Every request may also carry
"content": the installed content version and the manifest serial it came
from (nsr_content.content_descriptor). Host/room compare it before the
fingerprint, so the outdated side gets "Click Check for updates" instead
of a bare fingerprint mismatch; the reject then carries "action"
("update_content" / "host_update_content", raised here as RoomRejected).
The field is optional both ways: older launchers and room servers ignore
it, and without it on both sides only the fingerprint decides (it still
catches every content mismatch, just with the generic message).
"""
from __future__ import annotations

import json
import secrets
import socket
import threading
from dataclasses import dataclass, field
from typing import Callable

import nsr_content

PORT = 51515
MAX_PLAYERS = 4
# 2: hello/start carry "mode" (rollback release). Direct-IP lobbies run in the
# players' own launchers, so bumping it just makes a lockstep-era launcher on
# either side get a clear "different version" message.
PROTOCOL_VERSION = 2
MODES = ("lockstep", "rollback")
RELEASE_MODE = "rollback"  # what this launcher's image plays (nsr_patcher.NETPLAY_MODE)


def _check_mode(mode: str) -> str:
    if mode not in MODES:
        raise ValueError("Unknown netplay mode.")
    return mode


def _agreed_mode(msg: dict, expected: str, who: str) -> str:
    """Mode from a start message. A missing mode (room servers older than the
    rollback release don't echo it) means ours: the fingerprint check already
    proved everyone runs the same image, and the image fixes the mode."""
    mode = str(msg.get("mode", expected))
    if mode not in MODES:
        raise ConnectionError(f"{who} selected an unknown netplay mode.")
    if mode != expected:
        raise ConnectionError(f"{who} started a {mode} game, but this launcher plays {expected}. "
                              "Everyone needs the same NFLStreetRollback download.")
    return mode


class RoomRejected(ConnectionError):
    """Refused by the host/room. action names what fixes it (e.g.
    nsr_content.UPDATE_ACTION) when the server says so, else ""."""

    def __init__(self, reason: str, action: str = ""):
        super().__init__(reason)
        self.action = action if action in (nsr_content.UPDATE_ACTION, nsr_content.HOST_UPDATE_ACTION) else ""


def _rejected(msg: dict, default: str) -> RoomRejected:
    return RoomRejected(str(msg.get("reason", default))[:400], str(msg.get("action", "")))


def _with_content(msg: dict, content: dict | None) -> dict:
    if content is not None:
        msg["content"] = nsr_content.content_descriptor(content["version"], content["serial"])
    return msg


@dataclass
class StartInfo:
    player: int          # our player number (0 = host)
    players: int         # total players
    delay: int           # input delay in frames
    host_address: str    # where to send game traffic ("" for the host itself;
                         # unused with a rendezvous: hole punching finds peers)
    rendezvous: str = ""  # "host:port" of the rendezvous server (room codes)
    room: str = ""        # room id for UDP registration
    key: str = ""         # our per-player secret for UDP registration
    seed: int = 0x13579BDF  # per-match RNG seed, identical for every player
    mode: str = RELEASE_MODE  # "rollback": stage-4 lockstep menus + rollback matches
    room_code: str = ""   # "NSRB-4K7QX" (display only; telemetry header)
    telemetry_url: str = ""    # ingest endpoint granted by the room server
    telemetry_token: str = ""  # per-session/per-player token (docs/telemetry.md)


def _send(sock: socket.socket, msg: dict) -> None:
    sock.sendall((json.dumps(msg) + "\n").encode())


def _lines(sock: socket.socket):
    buf = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            return
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if line.strip():
                yield json.loads(line)


@dataclass
class _Joiner:
    name: str
    sock: socket.socket
    address: str


@dataclass
class LobbyHost:
    """Accepts up to 3 joiners. on_change(names) fires whenever the roster
    changes (names[0] is the host). Call start() to begin; it returns our
    StartInfo after telling every joiner theirs."""

    name: str
    fingerprint: str
    on_change: Callable[[list[str]], None] = lambda names: None
    on_reject: Callable[[str, str], None] = lambda who, reason: None
    port: int = PORT
    bind: str = ""
    mode: str = RELEASE_MODE
    content: dict | None = None  # our nsr_content descriptor (None: don't compare)
    _joiners: list[_Joiner] = field(default_factory=list)
    _lock: threading.Lock = field(default_factory=threading.Lock)
    _server: socket.socket | None = None
    _closed: bool = False

    def open(self) -> None:
        self._server = socket.create_server((self.bind, self.port), reuse_port=False)
        self._server.settimeout(0.5)
        threading.Thread(target=self._accept_loop, daemon=True).start()
        self._notify()

    def names(self) -> list[str]:
        with self._lock:
            return [self.name] + [j.name for j in self._joiners]

    def _notify(self) -> None:
        names = self.names()
        with self._lock:
            joiners = list(self._joiners)
        for j in joiners:
            try:
                _send(j.sock, {"type": "lobby", "players": names})
            except OSError:
                pass
        self.on_change(names)

    def _accept_loop(self) -> None:
        while not self._closed:
            try:
                sock, addr = self._server.accept()
            except (TimeoutError, socket.timeout):
                continue
            except OSError:
                return
            threading.Thread(target=self._handle, args=(sock, addr[0]), daemon=True).start()

    def _handle(self, sock: socket.socket, address: str) -> None:
        sock.settimeout(10)
        try:
            hello = next(_lines(sock))
        except (StopIteration, OSError, ValueError):
            sock.close()
            return
        name = str(hello.get("name") or "Player")[:24]
        reason, action = None, ""
        content = nsr_content.room_content_reason(self.content, hello.get("content"))
        if hello.get("type") != "hello" or hello.get("version") != PROTOCOL_VERSION:
            reason = ("Your launcher is a different version from the host's. "
                      "Everyone needs the same NFLStreetRollback download.")
        elif hello.get("mode", "lockstep") != self.mode:
            reason = (f"Your launcher plays {hello.get('mode', 'lockstep')} netplay; this lobby is "
                      f"{self.mode}. Everyone needs the same NFLStreetRollback download.")
        elif content:
            reason, action = content
        elif hello.get("fingerprint") != self.fingerprint:
            reason = (f"Game image mismatch: yours is {hello.get('fingerprint')}, the host's is "
                      f"{self.fingerprint}. Everyone needs the same Reborn version and patches.")
        with self._lock:
            if reason is None and len(self._joiners) + 1 >= MAX_PLAYERS:
                reason = "The lobby is full (4 players)."
            if reason is None:
                self._joiners.append(_Joiner(name, sock, address))
        if reason:
            self.on_reject(name, reason)  # before replying, so the host's UI is never behind
            try:
                _send(sock, {"type": "reject", "reason": reason, **({"action": action} if action else {})})
            finally:
                sock.close()
            return
        sock.settimeout(None)
        self._notify()
        # Watch for the joiner leaving before the game starts.
        try:
            for _ in _lines(sock):
                pass
        except OSError:
            pass
        with self._lock:
            still_here = any(j.sock is sock for j in self._joiners)
            self._joiners = [j for j in self._joiners if j.sock is not sock]
        if still_here and not self._closed:
            self._notify()

    def start(self, delay: int = 2, mode: str | None = None) -> StartInfo:
        """Start every peer with the lobby's netplay mode (mode, if given, must
        equal it: joiners were admitted for that mode)."""
        mode = _check_mode(mode or self.mode)
        if mode != self.mode:
            raise ValueError("This lobby was opened for a different netplay mode.")
        with self._lock:
            joiners = list(self._joiners)
            self._closed = True
        players = 1 + len(joiners)
        if players < 2:
            self._closed = False
            raise ValueError("Need at least one other player to start.")
        seed = secrets.randbits(32)  # per-match RNG seed, sent to everyone
        for k, j in enumerate(joiners, start=1):
            _send(j.sock, {"type": "start", "player": k, "players": players, "delay": delay,
                           "seed": seed, "mode": mode})
            j.sock.close()
        self._server.close()
        return StartInfo(0, players, delay, "", seed=seed, mode=mode)

    def close(self) -> None:
        self._closed = True
        with self._lock:
            joiners, self._joiners = self._joiners, []
        for j in joiners:
            try:
                _send(j.sock, {"type": "closed"})
                j.sock.close()
            except OSError:
                pass
        if self._server:
            self._server.close()


def join(host: str, name: str, fingerprint: str,
         on_lobby: Callable[[list[str]], None] = lambda names: None,
         cancel: threading.Event | None = None, port: int = PORT,
         mode: str = RELEASE_MODE, content: dict | None = None) -> StartInfo:
    """Connect to a host's lobby and wait for Start. Raises ConnectionError
    with a readable reason if refused, closed, or unreachable."""
    if ":" in host:
        host, p = host.rsplit(":", 1)
        port = int(p)
    try:
        sock = socket.create_connection((host, port), timeout=10)
    except OSError as e:
        raise ConnectionError(f"Couldn't reach {host}:{port} ({e.strerror or e}). Check the address, "
                              "and that the host forwarded TCP+UDP port 51515.") from e
    address = sock.getpeername()[0]
    sock.settimeout(0.5)
    _check_mode(mode)
    _send(sock, _with_content({"type": "hello", "name": name, "fingerprint": fingerprint,
                               "version": PROTOCOL_VERSION, "mode": mode}, content))
    buf = b""
    try:
        while True:
            if cancel and cancel.is_set():
                raise ConnectionError("Cancelled.")
            try:
                chunk = sock.recv(4096)
            except (TimeoutError, socket.timeout):
                continue
            if not chunk:
                raise ConnectionError("The host closed the lobby.")
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                msg = json.loads(line)
                kind = msg.get("type")
                if kind == "lobby":
                    on_lobby(list(msg["players"]))
                elif kind == "reject":
                    raise _rejected(msg, "Refused by the host.")
                elif kind == "closed":
                    raise ConnectionError("The host closed the lobby.")
                elif kind == "start":
                    agreed = _agreed_mode(msg, mode, "The host")
                    return StartInfo(int(msg["player"]), int(msg["players"]),
                                     int(msg.get("delay", 2)), address,
                                     seed=int(msg.get("seed", 0x13579BDF)), mode=agreed)
    finally:
        sock.close()


# --- room codes via the rendezvous server ------------------------------------
RENDEZVOUS_PORT = 51600
# Still 2 in the rollback release, on purpose: the deployed room server
# accepts exactly version 2 and ignores unknown fields, so new launchers keep
# working against it while it is upgraded. create/join/start now also carry
# "mode"; an upgraded server compares it and echoes it in "start", an old one
# ignores it (then the fingerprint check is what keeps a lockstep-image
# launcher out of a rollback-image room, and _agreed_mode falls back to ours).
RENDEZVOUS_VERSION = 2


def _split(addr: str, default_port: int) -> tuple[str, int]:
    if ":" in addr:
        host, port = addr.rsplit(":", 1)
        return host, int(port)
    return addr, default_port


@dataclass
class RoomHost:
    """Creates a room on the rendezvous server. open() returns the room code;
    on_change(names) fires on roster changes; start() returns our StartInfo."""

    rendezvous: str
    name: str
    fingerprint: str
    on_change: Callable[[list[str]], None] = lambda names: None
    on_error: Callable[[str], None] = lambda reason: None
    mode: str = RELEASE_MODE
    content: dict | None = None  # our nsr_content descriptor (sent with create)
    code: str = ""
    _sock: socket.socket | None = None
    _start: StartInfo | None = None
    _error: str = ""
    _started: threading.Event = field(default_factory=threading.Event)

    def open(self) -> str:
        host, port = _split(self.rendezvous, RENDEZVOUS_PORT)
        try:
            self._sock = socket.create_connection((host, port), timeout=10)
        except OSError as e:
            raise ConnectionError(f"Couldn't reach the room server ({self.rendezvous}): "
                                  f"{e.strerror or e}") from e
        _send(self._sock, _with_content({"type": "create", "name": self.name, "fingerprint": self.fingerprint,
                                         "version": RENDEZVOUS_VERSION, "mode": _check_mode(self.mode)},
                                        self.content))
        lines = _lines(self._sock)
        first = next(lines, None)
        if not first or first.get("type") != "room":
            raise ConnectionError((first or {}).get("reason", "The room server refused the room."))
        self.code = first["code"]
        self._sock.settimeout(None)
        threading.Thread(target=self._read, args=(lines,), daemon=True).start()
        return self.code

    def _read(self, lines) -> None:
        try:
            for msg in lines:
                kind = msg.get("type")
                if kind == "lobby":
                    self.on_change(list(msg["players"]))
                elif kind == "error":
                    self.on_error(msg.get("reason", "error"))
                elif kind == "start":
                    try:
                        self._start = _start_info(msg, self.rendezvous, self.mode)
                    except ConnectionError as e:
                        self._error = str(e)
                    self._started.set()
                    return
        except (OSError, ValueError):
            pass
        if not self._started.is_set():
            self.on_error("Lost the connection to the room server.")

    def start(self, delay: int = 2, timeout: float = 10, mode: str | None = None) -> StartInfo:
        """Ask the rendezvous service to start the room in its mode."""
        mode = _check_mode(mode or self.mode)
        if mode != self.mode:
            raise ValueError("This room was opened for a different netplay mode.")
        _send(self._sock, {"type": "start", "delay": delay, "mode": mode})
        if not self._started.wait(timeout):
            raise ConnectionError("The room server didn't confirm the start.")
        if self._error:
            raise ConnectionError(self._error)
        return self._start

    def close(self) -> None:
        if self._sock:
            # shutdown() first: a plain close() while the reader thread is
            # blocked in recv() may never send FIN, so the server (and the
            # joiners waiting in this room) wouldn't learn the host left.
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self._sock.close()
            except OSError:
                pass


def _start_info(msg: dict, rendezvous: str, expected_mode: str = RELEASE_MODE) -> StartInfo:
    room = str(msg["room"])
    # Older room servers didn't send a seed; the room id is shared by every
    # player, so deriving one from it still keeps all machines identical.
    seed = int(msg["seed"]) if "seed" in msg else int(room[:8], 16)
    mode = _agreed_mode(msg, expected_mode, "The room server")
    telemetry = msg.get("telemetry") if isinstance(msg.get("telemetry"), dict) else {}
    return StartInfo(int(msg["player"]), int(msg["players"]), int(msg.get("delay", 2)), "",
                     rendezvous=rendezvous, room=room, key=str(msg["key"]), seed=seed,
                     mode=mode, room_code=str(msg.get("code", ""))[:16],
                     telemetry_url=str(telemetry.get("url", "")),
                     telemetry_token=str(telemetry.get("token", "")))


def join_room(rendezvous: str, code: str, name: str, fingerprint: str,
              on_lobby: Callable[[list[str]], None] = lambda names: None,
              cancel: threading.Event | None = None, mode: str = RELEASE_MODE,
              content: dict | None = None) -> StartInfo:
    """Join a room by code and wait for the host's Start. Raises
    ConnectionError with a readable reason if refused, closed, or unreachable."""
    host, port = _split(rendezvous, RENDEZVOUS_PORT)
    try:
        sock = socket.create_connection((host, port), timeout=10)
    except OSError as e:
        raise ConnectionError(f"Couldn't reach the room server ({rendezvous}): {e.strerror or e}") from e
    sock.settimeout(0.5)
    _check_mode(mode)
    _send(sock, _with_content({"type": "join", "code": code, "name": name, "fingerprint": fingerprint,
                               "version": RENDEZVOUS_VERSION, "mode": mode}, content))
    buf = b""
    try:
        while True:
            if cancel and cancel.is_set():
                raise ConnectionError("Cancelled.")
            try:
                chunk = sock.recv(4096)
            except (TimeoutError, socket.timeout):
                continue
            if not chunk:
                raise ConnectionError("The room server closed the connection.")
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                msg = json.loads(line)
                kind = msg.get("type")
                if kind == "lobby":
                    on_lobby(list(msg["players"]))
                elif kind == "reject":
                    raise _rejected(msg, "Refused.")
                elif kind == "closed":
                    raise ConnectionError(msg.get("reason", "Refused."))
                elif kind == "start":
                    return _start_info(msg, rendezvous, mode)
    finally:
        sock.close()
