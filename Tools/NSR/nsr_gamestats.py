"""nsr_gamestats.py: find NFL Street Reborn's per-game stats files and upload them.

Reborn's own Dolphin writes one JSON capture per finished game (schema
nsr.game.v1) into `GameStats/` in its Dolphin user folder, and Reborn's
launcher moves captures into `<data root>/stats/queue/` for its Stats page.
The NFLStreetRollback launcher uses this module to send new captures to Mason's
telemetry server (POST /v1/games, docs/stats.md), so everyone's games land in
one place without anybody copying files around.

Read-only on Reborn: files are only listed and read, never moved or deleted
(Reborn's launcher manages its queue itself). What has been sent is remembered
by content hash in `nsr-gamestats.json` next to the launcher, so moving a file
from GameStats/ into the queue never uploads it twice; the server dedupes too.

Pure helpers plus a small Uploader with an injectable HTTP poster, so tests
need no network. Stdlib only (bundled into "NFLStreetRollback.exe").
"""
from __future__ import annotations

import gzip
import hashlib
import json
import os
import secrets
import tempfile
import time
import urllib.error
import urllib.request

import nsr_controllers as controllers

STATE_FILE = "nsr-gamestats.json"
MAX_FILE_BYTES = 1 << 20
MAX_FILES_PER_SCAN = 5000
MAX_LEDGER = 20000
UPLOADS_PER_PASS = 200            # a backlog drains over a few passes
INTERVAL_S = 300                  # background pass while the launcher is open
RETRY_BACKOFF_S = (60, 300, 900, 3600)


# --- where captures live ----------------------------------------------------------
def stats_dirs(install: str | None, env=os.environ) -> list[str]:
    """Existing folders that may hold captures (see module doc); nothing is created."""
    found = [os.path.join(u, "GameStats") for u in controllers.candidate_user_dirs(install, env)]
    for root in controllers.data_roots(install, env):
        found += [os.path.join(root, "stats", "queue"), os.path.join(root, "GameStats")]
    return [f for f in dict.fromkeys(os.path.normpath(f) for f in found) if os.path.isdir(f)]


def scan(dirs: list[str], limit: int = MAX_FILES_PER_SCAN):
    """Yield .json paths under the folders, newest folders' files in name order."""
    count = 0
    for d in dirs:
        for dirpath, dirnames, filenames in os.walk(d):
            dirnames.sort()
            for name in sorted(filenames):
                if name.lower().endswith(".json"):
                    count += 1
                    if count > limit:
                        return
                    yield os.path.join(dirpath, name)


def read_capture(path: str) -> tuple[str, dict] | None:
    """(digest, capture object) for an nsr.game.v1 file; ({}, raw digest) for others.

    The digest of a capture is the SHA-256 of its canonical JSON (sorted keys,
    compact), the same content hash the server dedupes on, so a GameStats file
    and its wrapped copy in Reborn's queue count as one. Only a light check
    happens here (JSON object with an nsr.game.v1 schema, possibly nested in a
    queue entry); the server validates fully. None: unreadable or too large.
    """
    try:
        if os.path.getsize(path) > MAX_FILE_BYTES:
            return None
        with open(path, "rb") as f:
            data = f.read(MAX_FILE_BYTES + 1)
    except OSError:
        return None
    digest = hashlib.sha256(data).hexdigest()
    try:
        obj = json.loads(data.decode("utf-8-sig"))
    except (UnicodeDecodeError, ValueError):
        return digest, {}
    if isinstance(obj, dict) and "schema" not in obj:
        obj = next((v for k, v in obj.items() if k in ("capture", "game", "payload", "data", "body")
                    and isinstance(v, dict) and "schema" in v), obj)
    if not isinstance(obj, dict) or not str(obj.get("schema", "")).startswith("nsr.game.v1"):
        return digest, {}
    canonical = json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    return hashlib.sha256(canonical).hexdigest(), obj


def games_url(telemetry_url: str) -> str:
    """https://host/v1/telemetry → https://host/v1/games (same service)."""
    url = (telemetry_url or "").strip().rstrip("/")
    if not url:
        return ""
    for suffix in ("/v1/telemetry", "/v1/games"):
        if url.endswith(suffix):
            return url[: -len(suffix)] + "/v1/games"
    return url + "/v1/games"


def http_post(url: str, body: bytes, headers: dict, timeout: float = 15.0) -> tuple[int, dict]:
    """POST and return (status, JSON body); status 0 when the server is unreachable."""
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}
    except (OSError, ValueError):
        return 0, {}


# --- the uploader -----------------------------------------------------------------
class Uploader:
    """One pass = find new captures, make sure we hold an upload token, send them.

    Identity: a random install id (128 bits, created on first use) is this
    launcher install; the server derives the public user id from it and
    signs an upload token for it (/v1/games/register). The display name is
    the launcher's name box, sent with every upload so renames follow.
    """

    def __init__(self, folder: str, post=http_post, now=time.time):
        self.folder = folder
        self.path = os.path.join(folder, STATE_FILE)
        self._post = post
        self._now = now
        self.state = self._load()
        self.failures = 0
        self.next_try = 0.0

    def _load(self) -> dict:
        try:
            with open(self.path, encoding="utf-8") as f:
                state = json.load(f)
            if not isinstance(state, dict):
                raise ValueError
        except (OSError, ValueError):
            state = {}
        if not isinstance(state.get("install_id"), str) or len(state["install_id"]) != 32:
            state["install_id"] = secrets.token_hex(16)
        state.setdefault("sent", {})
        return state

    def save(self) -> None:
        sent = self.state["sent"]
        if len(sent) > MAX_LEDGER:  # keep the newest entries
            for k in sorted(sent, key=lambda k: sent[k].get("t", 0))[: len(sent) - MAX_LEDGER]:
                sent.pop(k)
        try:
            fd, tmp = tempfile.mkstemp(prefix=".gamestats-", dir=self.folder)
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(self.state, f)
            os.replace(tmp, self.path)
        except OSError:
            pass

    def remember_server(self, telemetry_url: str = "", room_token: str = "") -> None:
        """A room start told us the server and gave a token usable as registration proof."""
        if telemetry_url:
            self.state["url"] = games_url(telemetry_url)
        if room_token:
            self.state["proof"] = room_token
            self.state["proof_t"] = self._now()
        self.save()

    def _register(self, url: str, name: str, shared_key: str) -> str | None:
        proofs = [p for p in (self.state.get("proof") if self._now() - self.state.get("proof_t", 0) < 8 * 3600
                              else "", shared_key, "") if p is not None]
        for proof in dict.fromkeys(proofs):
            headers = {"Content-Type": "application/json"}
            if proof:
                headers["Authorization"] = f"Bearer {proof}"
            body = json.dumps({"install_id": self.state["install_id"], "name": name}).encode()
            status, reply = self._post(url + "/register", body, headers)
            if status == 200 and reply.get("token"):
                self.state.update(token=reply["token"], token_exp=reply.get("expires", 0),
                                  user_id=reply.get("user_id", ""))
                self.save()
                return reply["token"]
            if status == 0:
                return None
        return None

    def pending(self, dirs: list[str]) -> list[tuple[str, str, dict]]:
        out, seen = [], set()
        for path in scan(dirs):
            got = read_capture(path)
            if got is None:
                continue
            digest, cap = got
            if digest in self.state["sent"] or digest in seen:
                continue
            seen.add(digest)
            if not cap:
                self.state["sent"][digest] = {"t": int(self._now()), "r": "not-a-capture"}
                continue
            out.append((path, digest, cap))
        return out

    def run_once(self, dirs: list[str], name: str, url: str = "", shared_key: str = "",
                 force: bool = False) -> dict:
        """Upload what is new. Returns a status dict for the launcher UI."""
        url = url or self.state.get("url", "")
        todo = self.pending(dirs)
        result = {"state": "ok", "sent": 0, "pending": len(todo), "rejected": 0,
                  "folders": len(dirs), "total_sent": 0}
        if not url:
            result["state"] = "no-server"
        elif not force and self._now() < self.next_try:
            result["state"] = "waiting"
        else:
            result.update(self._send(todo[:UPLOADS_PER_PASS], url, name, shared_key))
        result["pending"] = len(todo) - result["sent"] - result["rejected"]
        result["total_sent"] = sum(1 for v in self.state["sent"].values() if v.get("r") in ("game", "perspective", "duplicate"))
        self.save()
        return result

    def _send(self, todo, url, name, shared_key) -> dict:
        sent = rejected = 0
        token = self.state.get("token") if self.state.get("token_exp", 0) > self._now() + 3600 else None
        for path, digest, cap in todo:
            if not token:
                token = self._register(url, name, shared_key)
                if not token:
                    return self._fail(sent, rejected, "no-token")
            body = gzip.compress(json.dumps({"v": 1, "install_id": self.state["install_id"], "name": name,
                                             "file": os.path.basename(path)[:120], "capture": cap}).encode())
            headers = {"Content-Type": "application/json", "Content-Encoding": "gzip",
                       "Authorization": f"Bearer {token}"}
            status, reply = self._post(url, body, headers)
            if status == 200:
                self.state["sent"][digest] = {"t": int(self._now()), "r": reply.get("result", "ok")}
                sent += 1
            elif status in (400, 413, 422):  # this file will never be accepted: stop retrying it
                self.state["sent"][digest] = {"t": int(self._now()), "r": "rejected"}
                rejected += 1
            elif status in (401, 403):  # token revoked/expired/secret rotated: register again
                self.state.pop("token", None)
                token = None
                return self._fail(sent, rejected, "unauthorized")
            else:  # offline, 429, 5xx: keep it for the next pass
                return self._fail(sent, rejected, "offline" if status == 0 else f"http-{status}")
        self.failures = 0
        self.next_try = 0.0
        return {"sent": sent, "rejected": rejected, "state": "ok"}

    def _fail(self, sent, rejected, why) -> dict:
        self.next_try = self._now() + RETRY_BACKOFF_S[min(self.failures, len(RETRY_BACKOFF_S) - 1)]
        self.failures += 1
        return {"sent": sent, "rejected": rejected, "state": why}
