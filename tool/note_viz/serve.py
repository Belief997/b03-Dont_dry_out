"""Dev server for the note review view.

  python build.py --serve [--port 8706]

- GET  /              -> the built, self-contained HTML
- GET  /api/events    -> text/event-stream; a `reload` event carries a fresh
                         model whenever the note or any of the .py/.js/.css
                         inputs change on disk
- POST /api/review    -> overwrite review/<NN>.json with the posted note list
                         (body: [{match, text, author}]); also triggers a rebuild

Only stdlib. Everything is served from 127.0.0.1.
"""
import io
import json
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import build

POLL = 0.6          # seconds between mtime scans
FIRST_WAIT = 2.0    # ignore editor write bursts right after start


def _read(p):
    with io.open(p, encoding="utf-8") as f:
        return f.read()


class Watcher(object):
    """Snapshot of mtimes; yields a reason string when something changed.

    `out/` must NOT be watched: every rebuild rewrites the HTML there, so
    watching it would trigger the next rebuild forever."""

    def __init__(self, note_path):
        self.note = note_path
        self.dirs = [os.path.join(build.HERE, "web"), build.REVIEW]
        self.skip = {os.path.abspath(build.OUT), os.path.abspath(build.HERE)}
        self.snap = self.scan()

    def scan(self):
        out = {}
        paths = [self.note]
        for d in self.dirs:
            if not os.path.isdir(d):
                continue
            for dp, dns, fns in os.walk(d):
                if os.path.abspath(dp) in self.skip:
                    dns[:] = []
                    continue
                dns[:] = [x for x in dns if x != "__pycache__"]
                for f in fns:
                    if f.endswith((".md", ".js", ".css", ".html", ".json")):
                        paths.append(os.path.join(dp, f))
        # the python sources themselves, but not out/
        for f in os.listdir(build.HERE):
            if f.endswith(".py"):
                paths.append(os.path.join(build.HERE, f))
        for p in paths:
            try:
                out[p] = os.path.getmtime(p)
            except OSError:
                pass
        return out

    def changed(self):
        cur = self.scan()
        if cur == self.snap:
            return None
        old, self.snap = self.snap, cur
        touched = [p for p, t in cur.items() if old.get(p) != t]
        touched += [p for p in old if p not in cur]
        if not touched:
            return None
        # the note itself is the interesting one; config/asset edits also reload
        return os.path.basename(self.note) if self.note in touched else \
            ", ".join(sorted(os.path.basename(p) for p in touched)[:3])


class Hub(object):
    def __init__(self):
        self.clients = []
        self.lock = threading.Lock()
        self.body = None
        self.rev = 0

    def publish(self, payload, reason):
        with self.lock:
            self.body = json.dumps({"reason": reason, "rev": self.rev}, ensure_ascii=False)
            self.rev += 1
            dead = []
            for q in self.clients:
                try:
                    q.put(payload)
                except Exception:
                    dead.append(q)
            for q in dead:
                self.clients.remove(q)

    def add(self):
        import queue
        q = queue.Queue(maxsize=4)
        with self.lock:
            self.clients.append(q)
        return q

    def remove(self, q):
        with self.lock:
            if q in self.clients:
                self.clients.remove(q)


HUB = Hub()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    note_path = None
    out_path = None
    model_json = b""
    rebuild = None

    def log_message(self, fmt, *a):
        pass

    # ---------------------------------------------------------------- helpers
    def _send(self, code, body, ctype="text/html; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, json.dumps(obj, ensure_ascii=False),
                   "application/json; charset=utf-8")

    # ---------------------------------------------------------------- routes
    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            try:
                html = io.open(self.out_path, encoding="utf-8").read()
            except IOError:
                return self._send(500, "out/ missing; run build.py first")
            return self._send(200, html)
        if path == "/api/events":
            return self._events()
        if path == "/api/model":
            return self._send(200, Handler.model_json, "application/json; charset=utf-8")
        if path == "/favicon.ico":
            return self._send(204, b"")
        return self._send(404, "not found")

    def do_POST(self):
        path = self.path.split("?")[0]
        if path != "/api/review":
            return self._json(404, {"error": "not found"})
        try:
            n = int(self.headers.get("content-length") or 0)
            raw = self.rfile.read(n) if n else b"[]"
            notes = json.loads(raw.decode("utf-8"))
            if not isinstance(notes, list):
                raise ValueError("expected a list")
        except Exception as e:                       # noqa: BLE001 - report to UI
            return self._json(400, {"error": str(e)})

        stem = os.path.splitext(os.path.basename(Handler.note_path))[0]
        target = os.path.join(build.REVIEW, stem[:2] + ".json")
        clean = []
        for x in notes:
            if not isinstance(x, dict) or not x.get("match") or not x.get("text"):
                continue
            clean.append({"match": str(x["match"]),
                          "text": str(x["text"]),
                          "author": str(x.get("author") or "reviewer")})
        try:
            if not os.path.isdir(build.REVIEW):
                os.makedirs(build.REVIEW)
            tmp = target + ".tmp"
            with io.open(tmp, "w", encoding="utf-8", newline="\n") as f:
                f.write(json.dumps(clean, ensure_ascii=False, indent=2))
            os.replace(tmp, target)
        except Exception as e:                       # noqa: BLE001
            return self._json(500, {"error": "write failed: %s" % e})

        Handler.rebuild(reason="review/%s.json" % stem[:2])
        return self._json(200, {"ok": True, "count": len(clean), "file": target})

    # ---------------------------------------------------------------- sse
    def _events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        q = HUB.add()
        try:
            self.wfile.write(b"event: open\ndata: {}\n\n")
            self.wfile.flush()
            while True:
                try:
                    payload = q.get(timeout=15)
                except Exception:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
                    continue
                self.wfile.write(b"event: reload\ndata: " + payload.encode("utf-8") + b"\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            HUB.remove(q)
            self.close_connection = True


def _watch_loop(note_path, out_path, build_fn):
    w = Watcher(note_path)
    time.sleep(FIRST_WAIT)
    while True:
        time.sleep(POLL)
        reason = w.changed()
        if reason is None:
            continue
        # debounce editor write bursts; fold anything that landed meanwhile
        time.sleep(0.25)
        w.changed()
        out, model = build_fn()
        payload = json.dumps(_slim(model), ensure_ascii=False)
        HUB.publish(payload, reason)
        print("  rebuilt (%s) -> %d issues" % (reason, len(model["issues"])))


def _slim(model):
    """Everything the browser needs to re-render after a save."""
    keep = ("meta", "lines", "blocks", "secs", "refs", "cmds", "unsol", "status",
            "ovs", "reqs", "checklist", "decisions", "code", "cites", "issues",
            "dupes", "review")
    return {k: model[k] for k in keep if k in model}


def run(note_path, out_path, port, build_fn):
    Handler.note_path = note_path
    Handler.out_path = out_path
    Handler.rebuild = lambda reason=None: build_fn()
    _, m = build_fn()
    Handler.model_json = json.dumps(_slim(m), ensure_ascii=False,
                                    separators=(",", ":")).encode("utf-8")

    t = threading.Thread(target=_watch_loop, args=(note_path, out_path, build_fn),
                         daemon=True)
    t.start()

    httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    httpd.daemon_threads = True
    print("serving http://127.0.0.1:%d/   (Ctrl-C 停止)" % port)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        httpd.server_close()
