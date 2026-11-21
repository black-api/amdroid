#!/usr/bin/env python3
"""store-server - the Am-Droid App Store server.

Serves the catalog and the packages the App Store app downloads:

  GET /catalog      the catalog: "STORE1" then one block per app ("@ID" and key=value lines)
  GET /pkg/<ID>     the app's .adp package, built from examples/apps/<dir> by adp-build

An app is listed when its project directory has a store.meta (category, color, tagline,
about, featured) next to its app.manifest. Packages are rebuilt when their sources change.

In QEMU's user network the host is 10.0.2.2, which is where the App Store looks by default.

Usage: tools/store-server.py [--port 8642] [--apps examples/apps]
"""
import argparse, http.server, os, subprocess, sys, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lock = threading.Lock()


def read_kv(path):
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def build(appdir, outdir):
    """The app's .adp, rebuilt if any source file is newer."""
    man = read_kv(os.path.join(appdir, "app.manifest"))
    out = os.path.join(outdir, man["id"] + ".ADP")
    newest = max(os.path.getmtime(os.path.join(appdir, f)) for f in os.listdir(appdir))
    if not os.path.exists(out) or os.path.getmtime(out) < newest:
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "adp-build.py"), appdir, "-o", out],
                       check=True, stdout=subprocess.DEVNULL)
    return out


def catalog(appsdir, outdir):
    apps = []
    for d in sorted(os.listdir(appsdir)):
        p = os.path.join(appsdir, d)
        if not os.path.exists(os.path.join(p, "store.meta")):
            continue
        man = read_kv(os.path.join(p, "app.manifest"))
        meta = read_kv(os.path.join(p, "store.meta"))
        adp = build(p, outdir)
        e = dict(man)
        e.update(meta)
        e["size"] = str(os.path.getsize(adp))
        apps.append(e)
    # featured first, then by name
    apps.sort(key=lambda e: (e.get("featured") != "1", e.get("name", "").lower()))
    lines = ["STORE1"]
    for e in apps:
        lines.append("@" + e["id"])
        for k in ("name", "version", "author", "icon", "desc", "perms", "requires", "category",
                  "color", "tagline", "about", "featured", "size"):
            if k in e:
                lines.append("%s=%s" % (k, e[k].replace("\n", " ")))
    lines.append("@END")
    return ("\n".join(lines) + "\n").encode()


def main():
    ap = argparse.ArgumentParser(description="Am-Droid App Store server")
    ap.add_argument("--port", type=int, default=8642)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--apps", default=os.path.join(ROOT, "examples", "apps"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "store"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    class H(http.server.BaseHTTPRequestHandler):
        def send(self, code, body, ctype="text/plain"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            try:
                with lock:
                    if self.path == "/catalog":
                        return self.send(200, catalog(a.apps, a.out))
                    if self.path.startswith("/pkg/"):
                        want = self.path[5:].upper()
                        for d in os.listdir(a.apps):
                            p = os.path.join(a.apps, d)
                            if os.path.exists(os.path.join(p, "store.meta")) and \
                               read_kv(os.path.join(p, "app.manifest")).get("id") == want:
                                with open(build(p, a.out), "rb") as f:
                                    return self.send(200, f.read(), "application/octet-stream")
                self.send(404, b"not found\n")
            except Exception as e:  # keep serving
                self.send(500, ("error: %s\n" % e).encode())

        def log_message(self, fmt, *args):
            sys.stderr.write("[store] " + (fmt % args) + "\n")

    srv = http.server.ThreadingHTTPServer((a.bind, a.port), H)
    print("App Store server on port %d, apps from %s" % (a.port, a.apps))
    srv.serve_forever()


if __name__ == "__main__":
    main()
