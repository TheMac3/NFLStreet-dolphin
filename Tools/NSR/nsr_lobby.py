#!/usr/bin/env python3
"""nsr_lobby.py: host/join lobby for NFLStreet netplay (2-4 players).

The launcher's "Host" and "Join" buttons use this. It only assigns player
numbers and agrees on settings; the game traffic itself is Dolphin's
(NSRNetplay, UDP, star topology: everyone talks to the host). The host's
router needs TCP *and* UDP port 51515 forwarded for internet play.

Protocol: one TCP connection per joining player to the host, newline-delimited
JSON messages.

  joiner -> host  {"type": "hello", "name": str, "fingerprint": str, "version": int}
  host -> joiner  {"type": "reject", "reason": str}                 (then closes)
  host -> all     {"type": "lobby", "players": [name, ...]}          (on any change;
                                                                    index = player number)
  host -> joiner  {"type": "start", "player": k, "players": n, "delay": d}
  host -> joiner  {"type": "closed"}                                (host cancelled)

The host is always player 0. Players keep their join order. A joiner whose
game image fingerprint differs from the host's is refused before anything
starts, since lockstep needs byte-identical images.
"""
from __future__ import annotations

import json
import socket
import threading
from dataclasses import dataclass, field
from typing import Callable

PORT = 51515
MAX_PLAYERS = 4
PROTOCOL_VERSION = 1


@dataclass
class StartInfo:
    player: int          # our player number (0 = host)
    players: int         # total players
    delay: int           # input delay in frames
    host_address: str    # where to send game traffic ("" for the host itself)


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
        reason = None
        if hello.get("type") != "hello" or hello.get("version") != PROTOCOL_VERSION:
            reason = "Your launcher is a different version from the host's."
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
                _send(sock, {"type": "reject", "reason": reason})
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

    def start(self, delay: int = 2) -> StartInfo:
        with self._lock:
            joiners = list(self._joiners)
            self._closed = True
        players = 1 + len(joiners)
        if players < 2:
            self._closed = False
            raise ValueError("Need at least one other player to start.")
        for k, j in enumerate(joiners, start=1):
            _send(j.sock, {"type": "start", "player": k, "players": players, "delay": delay})
            j.sock.close()
        self._server.close()
        return StartInfo(0, players, delay, "")

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
         cancel: threading.Event | None = None, port: int = PORT) -> StartInfo:
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
    _send(sock, {"type": "hello", "name": name, "fingerprint": fingerprint,
                 "version": PROTOCOL_VERSION})
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
                    raise ConnectionError(msg.get("reason", "Refused by the host."))
                elif kind == "closed":
                    raise ConnectionError("The host closed the lobby.")
                elif kind == "start":
                    return StartInfo(int(msg["player"]), int(msg["players"]),
                                     int(msg.get("delay", 2)), address)
    finally:
        sock.close()
