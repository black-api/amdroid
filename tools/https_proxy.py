#!/usr/bin/env python3
"""HTTPS for am-droid's browser.

The kernel speaks plain HTTP only (no TLS). This proxy runs on the host, fetches HTTPS
pages with full certificate checks, and hands them to the guest over plain HTTP on the
virtual network (QEMU's user networking reaches the host at 10.0.2.2).

Guest request:  GET /https/<host>[:port]/<path> HTTP/1.0   (or POST, with Content-Type and a body)
The proxy answers with the site's status and body. Cookies are the guest's: its Cookie header
goes to the site, and the site's Set-Cookie headers (from every redirect) come back to it. X-Final-URL names the page after any
redirects (so relative links resolve against it); on a failure it answers 502 with the reason
in X-Proxy-Error.

To keep pages fast it:
  - keeps TLS connections to each site open and reuses them (no handshake per image),
  - asks sites for gzip and unpacks it here (less to download; the guest gets plain bytes),
  - remembers stylesheets, scripts, images and fonts for 10 minutes (memory only),
  - passes the browser's Accept header on (so sites send image formats the guest decodes).

Run:  python3 tools/https_proxy.py          (listens on 127.0.0.1:8888)
      python3 tools/https_proxy.py --no-cache --port 8888
"""
import gzip
import json
import http.client
import http.server
import os
import re
import ssl
import sys
import threading
import time
import urllib.parse
import zlib
from collections import OrderedDict

PORT = 8888
MAX_BYTES = 32 * 1024 * 1024       # big pages and scripts (YouTube's app is ~11 MB)
MAX_REDIRECTS = 5
CACHE_BYTES = 256 * 1024 * 1024
CACHE_SECONDS = 600
CACHEABLE = ("text/css", "javascript", "image/", "font/", "application/font", "application/json")

CTX = ssl.create_default_context()  # verifies the certificate chain


class Pool:
    """Idle keep-alive connections per (host, port)."""

    def __init__(self):
        self.lock = threading.Lock()
        self.idle = {}

    def get(self, host, port, fresh=False):
        with self.lock:
            conns = self.idle.get((host, port))
            if fresh and conns:
                # the site dropped one kept-alive connection: the others are likely dead too
                for c in conns:
                    c.close()
                conns.clear()
            if conns:
                return conns.pop(), True
        return http.client.HTTPSConnection(host, port, timeout=25, context=CTX), False

    def put(self, host, port, conn):
        with self.lock:
            conns = self.idle.setdefault((host, port), [])
            if len(conns) < 6:
                conns.append(conn)
                return
        conn.close()


class Cache:
    """Static subresources by URL, least recently used dropped first."""

    def __init__(self, limit):
        self.lock = threading.Lock()
        self.items = OrderedDict()
        self.size = 0
        self.limit = limit

    def get(self, url):
        with self.lock:
            it = self.items.get(url)
            if not it:
                return None
            if time.time() - it[0] > CACHE_SECONDS:
                self.size -= len(it[4])
                del self.items[url]
                return None
            self.items.move_to_end(url)
            return it

    def put(self, url, code, ctype, final, body):
        if self.limit <= 0 or len(body) > self.limit // 8:
            return
        with self.lock:
            old = self.items.pop(url, None)
            if old:
                self.size -= len(old[4])
            self.items[url] = (time.time(), code, ctype, final, body)
            self.size += len(body)
            while self.size > self.limit and self.items:
                _, it = self.items.popitem(last=False)
                self.size -= len(it[4])


POOL = Pool()
CACHE = Cache(0 if "--no-cache" in sys.argv else CACHE_BYTES)


def unpack(body, encoding):
    encoding = (encoding or "").lower()
    if encoding == "gzip":
        return gzip.decompress(body)
    if encoding == "deflate":
        try:
            return zlib.decompress(body)
        except zlib.error:
            return zlib.decompress(body, -zlib.MAX_WBITS)
    return body


def fetch(url, accept, method="GET", body_out=None, ctype_out=None, cookie=None, set_cookies=None):
    """GET (or POST) url over a pooled TLS connection, following redirects. Returns
    (status, content type, final url, body). The site's Set-Cookie headers are added to
    set_cookies; cookie is sent as the Cookie header (the guest picks it per site)."""
    for _ in range(MAX_REDIRECTS + 1):
        parts = urllib.parse.urlsplit(url)
        if parts.scheme != "https":
            raise ValueError("redirected to a non-HTTPS address: " + url)
        host, port = parts.hostname, parts.port or 443
        path = (parts.path or "/") + ("?" + parts.query if parts.query else "")
        headers = {"Host": parts.netloc, "User-Agent": "BRO-weser-proxy", "Accept": accept,
                   "Accept-Encoding": "gzip, deflate", "Connection": "keep-alive"}
        if cookie:
            headers["Cookie"] = cookie
        if method != "GET":
            headers["Content-Type"] = ctype_out or "application/x-www-form-urlencoded"
            headers["Content-Length"] = str(len(body_out or b""))
        for attempt in range(2):
            conn, reused = POOL.get(host, port, fresh=attempt > 0)
            try:
                conn.request(method, path, body=body_out if method != "GET" else None, headers=headers)
                r = conn.getresponse()
                body = r.read(MAX_BYTES + 1)
                break
            except (http.client.HTTPException, OSError):
                conn.close()
                if attempt == 0:
                    continue                    # the site dropped the connection: once more on a new one
                raise
        status, rh = r.status, r.headers
        if r.will_close or len(body) > MAX_BYTES:
            conn.close()
        else:
            POOL.put(host, port, conn)
        body = unpack(body[:MAX_BYTES], rh.get("Content-Encoding"))
        if set_cookies is not None:
            set_cookies.extend(rh.get_all("Set-Cookie") or [])
        loc = rh.get("Location")
        if status in (301, 302, 303, 307, 308) and loc:
            url = urllib.parse.urljoin(url, loc)
            if status in (301, 302, 303) and method != "GET":
                method, body_out = "GET", None        # a form's answer: the browser GETs the new page
            continue
        return status, rh.get("Content-Type", "text/html"), url, body
    raise ValueError("too many redirects")



# ---- YouTube media: resolved here, demuxed and decoded in the guest ----
# The guest's browser plays YouTube itself: the reader page (youtube_reader.js) runs a
# Media Source Extensions player, fetches the real DASH streams (fragmented MP4 or WebM,
# H.264/VP9/AV1 video, AAC/Opus audio) by byte range, and appends them to SourceBuffers;
# Reel, the OS's media process, demuxes and decodes them. Nothing is converted here.
#
# What this proxy adds for YouTube is the part a browser does by running YouTube's player
# script, which the guest's engine cannot: turning the video id into stream URLs (signature
# and n-parameter descrambling, the PO token) with yt-dlp. The URLs stay here; the guest
# names a stream by id and itag, and its bytes are passed through untouched, like any HTTPS.
#
#   GET /ytinfo/<id>                    JSON: title, duration, the DASH formats (no URLs)
#   GET /ytm/<id>/<itag>?r=<a>-<b>      bytes a..b of that stream, as YouTube serves them
YT_INFO = {}                          # id -> (time, info for the guest, {itag: url})
YT_LOCK = threading.Lock()
YT_TTL = 3 * 3600                     # googlevideo URLs live about six hours


def yt_resolve(vid):
    with YT_LOCK:
        hit = YT_INFO.get(vid)
        if hit and time.time() - hit[0] < YT_TTL:
            return hit[1], hit[2]
    import yt_dlp
    opts = {"quiet": True, "no_warnings": True, "skip_download": True, "noplaylist": True}
    with yt_dlp.YoutubeDL(opts) as y:
        info = y.extract_info("https://www.youtube.com/watch?v=" + vid, download=False)
    formats, urls = [], {}
    for f in info.get("formats") or []:
        itag = str(f.get("format_id") or "")
        if f.get("protocol") != "https" or not f.get("url") or not itag.isdigit():
            continue                  # DASH over plain HTTPS only (no HLS, no DRC duplicates)
        ext = f.get("ext")
        vc, ac = f.get("vcodec") or "none", f.get("acodec") or "none"
        if vc != "none" and ac != "none":
            continue                  # muxed progressive formats: the player uses DASH
        kind = "video" if vc != "none" else "audio"
        container = "webm" if ext == "webm" else "mp4"
        codecs = vc if kind == "video" else ac
        if codecs == "vp9":
            codecs = "vp09.00.10.08"
        formats.append({"itag": itag, "kind": kind, "mime": "%s/%s" % (kind, container), "codecs": codecs,
                        "width": f.get("width") or 0, "height": f.get("height") or 0, "fps": f.get("fps") or 0,
                        "kbps": round(f.get("tbr") or 0), "size": f.get("filesize") or f.get("filesize_approx") or 0,
                        "hdr": (f.get("dynamic_range") or "SDR") != "SDR"})
        urls[itag] = f["url"]
    out = {"id": vid, "title": info.get("title") or vid, "duration": info.get("duration") or 0,
           "author": info.get("uploader") or "", "formats": formats}
    with YT_LOCK:
        YT_INFO[vid] = (time.time(), out, urls)
    return out, urls


def yt_prefetch(vid):
    try:
        yt_resolve(vid)
    except Exception as e:                          # noqa: BLE001 — Play will say why
        print("youtube: could not resolve %s: %s" % (vid, e), file=sys.stderr)


def yt_range(url, a, b):
    """Bytes a..b of a googlevideo stream: redirects followed, the occasional 503 retried."""
    if b - a + 1 > MAX_BYTES:
        b = a + MAX_BYTES - 1
    last = "no answer"
    for attempt in range(5):
        u = url
        try:
            for _ in range(MAX_REDIRECTS + 1):
                parts = urllib.parse.urlsplit(u)
                path = parts.path + ("?" + parts.query if parts.query else "")
                conn = http.client.HTTPSConnection(parts.hostname, parts.port or 443, timeout=25, context=CTX)
                conn.request("GET", path, headers={"Host": parts.netloc, "Range": "bytes=%d-%d" % (a, b),
                                                   "User-Agent": "Mozilla/5.0", "Accept": "*/*"})
                r = conn.getresponse()
                if r.status in (301, 302, 303, 307, 308) and r.getheader("Location"):
                    u = urllib.parse.urljoin(u, r.getheader("Location"))
                    conn.close()
                    continue
                body = r.read()
                conn.close()
                if r.status in (200, 206):
                    if r.status == 200 and a:
                        body = body[a:b + 1]
                    return r.status, body
                last = "%d from %s" % (r.status, parts.hostname)
                if r.status < 500 and r.status != 429:
                    return r.status, b""
                break
        except (OSError, http.client.HTTPException) as e:
            last = str(e)
        time.sleep(0.5 * (attempt + 1))
    raise RuntimeError("the stream did not answer: " + last)

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "am-droid-https-proxy"
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("proxy: " + (fmt % args) + "\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.do_GET(method="POST", body=self.rfile.read(n) if n else b"")

    def do_GET(self, method="GET", body=None):
        if self.path.startswith("/ytinfo/") or self.path.startswith("/ytm/"):
            self.serve_youtube()
            return
        prefix = "/https/"
        if not self.path.startswith(prefix):
            self.send_error(404, "use /https/<host>/<path>, /ytinfo/<id> or /ytm/<id>/<itag>?r=a-b")
            return
        rest = self.path[len(prefix):]
        host_part, _, path = rest.partition("/")
        if not host_part:
            self.send_error(400, "missing host")
            return
        url = "https://" + host_part + "/" + path
        # a YouTube watch page: start resolving its streams now, so Play finds them ready
        m = re.match(r"(?:www\.|m\.)?youtube\.com/(?:watch\?(?:.*&)?v=|shorts/)([\w-]{11})", rest)
        if m:
            threading.Thread(target=yt_prefetch, args=(m.group(1),), daemon=True).start()
        accept = self.headers.get("Accept") or "text/html,text/plain;q=0.9,*/*;q=0.5"
        cookie = self.headers.get("Cookie")
        key = accept.split(",")[0] + " " + url
        hit = CACHE.get(key) if method == "GET" else None
        if hit:
            _, code, ctype, final, body = hit
            self.answer(code, ctype, final, body, "HIT")
            return
        set_cookies = []
        try:
            code, ctype, final, body = fetch(url, accept, method, body, self.headers.get("Content-Type"), cookie, set_cookies)
        except Exception as e:                      # bad certificate, DNS, timeout...
            reason = str(getattr(e, "reason", e)) or e.__class__.__name__
            reason = " ".join(reason.split())[:160]
            self.send_response(502)
            self.send_header("X-Proxy-Error", reason.encode("ascii", "replace").decode())
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        if code == 200 and method == "GET" and not set_cookies and any(t in ctype.lower() for t in CACHEABLE):
            CACHE.put(key, code, ctype, final, body)
        self.answer(code, ctype, final, body, "MISS", set_cookies)

    def serve_youtube(self):
        parsed = urllib.parse.urlparse(self.path)
        parts = parsed.path.strip("/").split("/")
        try:
            if parts[0] == "ytinfo" and len(parts) == 2:
                info, _ = yt_resolve(parts[1])
                self.answer(200, "application/json", self.path, json.dumps(info).encode(), "MISS")
                return
            if parts[0] == "ytm" and len(parts) == 3:
                _, urls = yt_resolve(parts[1])
                url = urls.get(parts[2])
                if not url:
                    self.send_error(404, "no such stream")
                    return
                rng = (urllib.parse.parse_qs(parsed.query).get("r") or ["0-1048575"])[0]
                a, _, b = rng.partition("-")
                a, b = int(a), int(b or (int(a) + 1048575))
                code, body = yt_range(url, a, b)
                self.answer(206 if code in (200, 206) else code, "application/octet-stream", self.path, body, "MISS")
                return
        except Exception as e:                      # noqa: BLE001 — the guest gets the reason
            reason = " ".join(str(e).split())[:160] or e.__class__.__name__
            print("youtube: " + reason, file=sys.stderr)
            self.send_response(502)
            self.send_header("X-Proxy-Error", reason.encode("ascii", "replace").decode())
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        self.send_error(400, "use /ytinfo/<id> or /ytm/<id>/<itag>?r=a-b")

    def answer(self, code, ctype, final_url, body, cache, set_cookies=()):
        self.send_response(code)
        for c in set_cookies:
            self.send_header("Set-Cookie", c)
        self.send_header("Content-Type", ctype)
        self.send_header("X-Final-URL", final_url)
        self.send_header("X-Proxy-Cache", cache)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    if "--port" in sys.argv:
        PORT = int(sys.argv[sys.argv.index("--port") + 1])
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    srv.daemon_threads = True
    print("https proxy on 127.0.0.1:%d%s" % (PORT, "" if CACHE.limit else " (no cache)"), file=sys.stderr)
    srv.serve_forever()
