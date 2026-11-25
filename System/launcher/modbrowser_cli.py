"""The launcher's mod browser talks to mb_cli.exe (Engine/Runtime/ui/mb_cli.cpp): one JSON object per line.
No tkinter here, so it is tested (tests/test_modbrowser_cli.py).  The sources, the safe install pipeline and the
version checks all live in the C++ helper; this module only builds command lines and reads answers."""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

SORTS = ("Trending", "Most popular", "Newest", "Recently updated")
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


class CliError(RuntimeError):
    pass


@dataclass
class Cli:
    exe: Path
    mods_root: Path
    settings: Path
    cache: Path
    exefs: Path | None = None
    mock: Path | None = None
    extra_env: dict = field(default_factory=dict)

    def available(self) -> bool:
        return Path(self.exe).is_file()

    def _args(self, cmd: str, **kw) -> list[str]:
        a = [str(self.exe), cmd, "--mods-root", str(self.mods_root), "--settings", str(self.settings),
             "--cache", str(self.cache)]
        if self.exefs:
            a += ["--exefs", str(self.exefs)]
        if self.mock:
            a += ["--mock", str(self.mock)]
        for k, v in kw.items():
            if v is None:
                continue
            a += ["--" + k.replace("_", "-"), str(v)]
        return a

    def _env(self) -> dict:
        env = dict(os.environ)
        env.update(self.extra_env)
        return env

    def call(self, cmd: str, timeout: int = 120, **kw) -> dict:
        """One command, its last JSON line; CliError with the helper's message on failure."""
        try:
            r = subprocess.run(self._args(cmd, **kw), capture_output=True, timeout=timeout, env=self._env(),
                               creationflags=NO_WINDOW)
        except (OSError, subprocess.SubprocessError) as e:
            raise CliError(f"mod browser helper failed: {e}") from e
        lines = [ln for ln in r.stdout.decode("utf-8", "replace").splitlines() if ln.startswith("{")]
        if not lines:
            raise CliError((r.stderr.decode("utf-8", "replace") or f"no answer (exit {r.returncode})").strip()[-500:])
        last = json.loads(lines[-1])
        if last.get("type") == "error":
            raise CliError(last.get("error", "error"))
        return last

    def start_install(self, *, source: str | None = None, mod_id: str | None = None, file_id: str | None = None,
                      path: str | None = None, allow: bool = False) -> subprocess.Popen:
        """An install as a process: read progress lines with read_lines(); kill() cancels."""
        if path is not None:
            args = self._args("install-local", path=path, allow="1" if allow else None)
        else:
            args = self._args("install", source=source, id=mod_id, file_id=file_id or None, allow="1" if allow else None)
        return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=self._env(),
                                creationflags=NO_WINDOW)

    def thumb_path(self, url: str, width: int = 320) -> Path:
        key = url if width == 320 else f"{url}#{width}"
        return Path(self.cache) / "thumbs" / (hashlib.sha1(key.encode("utf-8")).hexdigest() + ".png")

    def thumbnail(self, source: str, url: str, width: int = 320) -> Path:
        """The picture as a PNG Tk can show, at most width pixels wide (made once, then cached)."""
        p = self.thumb_path(url, width)
        if p.is_file():
            return p
        p.parent.mkdir(parents=True, exist_ok=True)
        self.call("image", source=source, url=url, out=str(p), max_width=width, timeout=60)
        return p


def read_lines(proc: subprocess.Popen):
    """JSON objects from an install process as they come; the last one is the result (or an error)."""
    assert proc.stdout is not None
    for raw in proc.stdout:
        line = raw.decode("utf-8", "replace").strip()
        if line.startswith("{"):
            try:
                yield json.loads(line)
            except ValueError:
                continue
    proc.wait()


def version_newer(a: str, b: str) -> bool:
    """a > b, as the C++ side compares versions ("1.10" > "1.9")."""
    x, y = [int(n) for n in re.findall(r"\d+", a or "")], [int(n) for n in re.findall(r"\d+", b or "")]
    if not x or not y:
        return False
    n = max(len(x), len(y))
    x += [0] * (n - len(x))
    y += [0] * (n - len(y))
    return x > y


def compat_badge(c: dict, target: str) -> tuple[str, str]:
    """(text, colour kind) of the compatibility badge: good / warn / bad (spec: green / yellow / red)."""
    kind = (c or {}).get("compat", "unknown")
    if kind == "match":
        return f"v{target}", "good"
    if kind == "mismatch":
        return f"Wrong version ({c.get('detected') or '?'})", "bad"
    return "Unknown version", "warn"


# ---- Nexus Mods log-in --------------------------------------------------------------------------------------------
# Single sign-on as in Nexus' own mod manager: a websocket to sso.nexusmods.com, the player confirms in the browser,
# the socket hands back the API key.  Nexus gives that only to applications it has registered: NEXUS_APP_SLUG must be
# OUR slug from Nexus (never another manager's).  Until we have one, log_in falls back to the API-key page + paste.
NEXUS_APP_SLUG = ""
NEXUS_KEY_PAGE = "https://www.nexusmods.com/settings/api-keys"
NEXUS_UA = "LodeStar-Launcher/1.0"


def nexus_validate(key: str, timeout: float = 20) -> dict:
    """{"ok", "name", "premium", "error"} for an API key (GET /v1/users/validate.json; the key only goes to Nexus)."""
    import urllib.error
    import urllib.request
    key = (key or "").strip()
    if not key:
        return {"ok": False, "error": "no key"}
    rq = urllib.request.Request("https://api.nexusmods.com/v1/users/validate.json",
                                headers={"apikey": key, "Accept": "application/json", "User-Agent": NEXUS_UA,
                                         "Application-Name": "LodeStar", "Application-Version": "1.0.0"})
    try:
        with urllib.request.urlopen(rq, timeout=timeout) as r:
            d = json.loads(r.read().decode("utf-8", "replace"))
        return {"ok": True, "name": d.get("name", ""), "premium": bool(d.get("is_premium")), "error": ""}
    except urllib.error.HTTPError as e:
        return {"ok": False, "error": "Nexus did not accept the key" if e.code in (401, 403) else f"HTTP {e.code}"}
    except (OSError, ValueError) as e:
        return {"ok": False, "error": f"no connection to Nexus ({e})"}


def nexus_sso(open_url, timeout: float = 300, slug: str = NEXUS_APP_SLUG) -> str:
    """Nexus single sign-on: returns the API key once the player confirmed in the browser ("" if cancelled / timed
    out).  open_url(url) opens the browser.  Needs a registered application slug (see NEXUS_APP_SLUG)."""
    import base64
    import socket
    import ssl
    import struct
    import time as _t
    import uuid
    if not slug:
        raise RuntimeError("no Nexus application slug registered yet")
    host = "sso.nexusmods.com"
    sock = ssl.create_default_context().wrap_socket(socket.create_connection((host, 443), timeout=20),
                                                     server_hostname=host)
    wkey = base64.b64encode(os.urandom(16)).decode()
    sock.sendall((f"GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Key: {wkey}\r\nSec-WebSocket-Version: 13\r\nUser-Agent: {NEXUS_UA}\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(4096)
        if not chunk:
            raise OSError("the log-in service closed the connection")
        head += chunk
    if b" 101 " not in head.split(b"\r\n", 1)[0]:
        raise OSError("the log-in service refused the connection")
    buf = head.split(b"\r\n\r\n", 1)[1]

    def send(text: str) -> None:
        data = text.encode()
        mask = os.urandom(4)
        hdr = bytes([0x81]) + (bytes([0x80 | len(data)]) if len(data) < 126 else bytes([0x80 | 126]) + struct.pack(">H", len(data)))
        sock.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def recv() -> str | None:
        nonlocal buf
        while True:
            if len(buf) >= 2:
                n, off = buf[1] & 0x7F, 2
                if n == 126 and len(buf) >= 4:
                    n, off = struct.unpack(">H", buf[2:4])[0], 4
                elif n == 127 and len(buf) >= 10:
                    n, off = struct.unpack(">Q", buf[2:10])[0], 10
                if len(buf) >= off + n and not (buf[1] & 0x7F in (126, 127) and off == 2):
                    op, payload, buf = buf[0] & 0x0F, buf[off:off + n], buf[off + n:]
                    if op == 0x8:
                        return None
                    if op == 0x9:                       # ping -> pong
                        sock.sendall(bytes([0x8A, 0x80 | len(payload)]) + b"\0\0\0\0" + payload)
                        continue
                    if op == 0x1:
                        return payload.decode("utf-8", "replace")
                    continue
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                return ""
            if not chunk:
                return None
            buf += chunk

    sid = str(uuid.uuid4())
    send(json.dumps({"id": sid, "token": None, "protocol": 2}))
    open_url(f"https://www.nexusmods.com/sso?id={sid}&application={slug}")
    sock.settimeout(5)
    end = _t.time() + timeout
    try:
        while _t.time() < end:
            msg = recv()
            if msg is None:
                return ""
            if not msg:
                continue
            d = json.loads(msg)
            key = (d.get("data") or {}).get("api_key") if isinstance(d.get("data"), dict) else None
            if d.get("success") and key:
                return key
        return ""
    finally:
        sock.close()


# ---- GitHub repositories (the settings' Repos window) -------------------------------------------------------------
GITHUB_INDEX = Path(__file__).resolve().parent / "assets" / "github_index.json"   # Engine/Tools/github_index.py


def norm_repo(text: str) -> str:
    """"owner/repo" from what a player pastes: owner/repo, https://github.com/owner/repo[.git][/tree/...]; "" if none."""
    t = (text or "").strip().strip('"').strip()
    t = re.sub(r"^(https?://)?(www\.)?github\.com/", "", t, flags=re.I)
    t = t.split("?")[0].split("#")[0].strip("/")
    parts = [p for p in t.split("/") if p]
    if len(parts) < 2:
        return ""
    owner, repo = parts[0], re.sub(r"\.git$", "", parts[1], flags=re.I)
    ok = re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})", owner) and re.fullmatch(r"[A-Za-z0-9._-]{1,100}", repo)
    return f"{owner}/{repo}" if ok else ""


def builtin_repos(index_path: Path = GITHUB_INDEX) -> list[dict]:
    """The built-in repositories with mods for this game: [{repo, name, mods}] from the shipped index."""
    try:
        d = json.loads(Path(index_path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    return [{"repo": r["repo"], "name": r.get("name") or r["repo"], "mods": int(r.get("mods", 0))}
            for r in d.get("repos", []) if r.get("ok") and r.get("mods")]


def repo_rows(st: dict, builtin: list[dict]) -> list[dict]:
    """The Repos window's list: [{repo, name, builtin}] - the saved list in its order, then every built-in the player
    has not deleted (so repositories a newer index adds show up too)."""
    names = {k.lower(): v for k, v in (st.get("github_names") or {}).items()}
    removed = {r.lower() for r in st.get("github_removed") or []}
    bset = {b["repo"].lower(): b for b in builtin}
    saved = [norm_repo(r) for r in st.get("github_repos") or []]
    saved = [r for i, r in enumerate(saved) if r and r.lower() not in {x.lower() for x in saved[:i]}]
    rows = [{"repo": r, "builtin": r.lower() in bset} for r in saved]
    have = {r["repo"].lower() for r in rows}
    for b in builtin:                       # every built-in the player has not deleted (new index entries too)
        k = b["repo"].lower()
        if k not in have and k not in removed:
            rows.append({"repo": b["repo"], "builtin": True})
    for r in rows:
        r["name"] = names.get(r["repo"].lower()) or (bset[r["repo"].lower()]["name"] if r["builtin"] else r["repo"])
    return rows


def apply_repo_rows(st: dict, rows: list[dict], builtin: list[dict]) -> dict:
    """The settings after the Repos window: github_repos (searched, in order), github_names (only names that differ
    from the default), github_removed (built-ins no longer in the list)."""
    bset = {b["repo"].lower(): b for b in builtin}
    st["github_repos"] = [r["repo"] for r in rows]
    st["github_names"] = {r["repo"]: r["name"] for r in rows
                          if r.get("name") and r["name"] != (bset[r["repo"].lower()]["name"] if r["repo"].lower() in bset
                                                             else r["repo"])}
    have = {r["repo"].lower() for r in rows}
    st["github_removed"] = sorted(b["repo"] for b in builtin if b["repo"].lower() not in have)
    return st


def load_settings(path: Path) -> dict:
    try:
        d = json.loads(Path(path).read_text(encoding="utf-8"))
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def save_settings(path: Path, d: dict) -> None:
    """Settings/modbrowser.json in the format the helper reads (API keys stay in this file only)."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def fmt_count(n: int) -> str:
    n = int(n or 0)
    if n >= 1_000_000:
        return f"{n / 1e6:.1f}M"
    if n >= 10_000:
        return f"{n / 1e3:.0f}k"
    if n >= 1000:
        return f"{n / 1e3:.1f}k"
    return str(n)


def fmt_size(n: int) -> str:
    n = int(n or 0)
    if not n:
        return "?"
    if n >= 1 << 30:
        return f"{n / (1 << 30):.1f} GB"
    if n >= 1 << 20:
        return f"{n / (1 << 20):.1f} MB"
    return f"{n / 1024:.0f} KB"
