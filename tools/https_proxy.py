#!/usr/bin/env python3
"""HTTPS for am-droid's browser.

The kernel speaks plain HTTP only (no TLS). This proxy runs on the host, fetches HTTPS
pages with full certificate checks, and hands them to the guest over plain HTTP on the
virtual network (QEMU's user networking reaches the host at 10.0.2.2).

Guest request:  GET /https/<host>[:port]/<path> HTTP/1.0
The proxy answers with the site's status and body. X-Final-URL names the page after any
redirects (so relative links resolve against it); on a failure it answers 502 with the reason
in X-Proxy-Error.

Run:  python3 tools/https_proxy.py          (listens on 127.0.0.1:8888)
"""
import http.server
import ssl
import sys
import urllib.error
import urllib.request

PORT = 8888
MAX_BYTES = 1024 * 1024


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "am-droid-https-proxy"

    def log_message(self, fmt, *args):
        sys.stderr.write("proxy: " + (fmt % args) + "\n")

    def do_GET(self):
        prefix = "/https/"
        if not self.path.startswith(prefix):
            self.send_error(404, "use /https/<host>/<path>")
            return
        rest = self.path[len(prefix):]
        host_part, _, path = rest.partition("/")
        if not host_part:
            self.send_error(400, "missing host")
            return
        url = "https://" + host_part + "/" + path
        ctx = ssl.create_default_context()          # verifies the certificate chain
        req = urllib.request.Request(url, headers={"User-Agent": "BRO-weser-proxy",
                                                   "Accept": "text/html,text/plain;q=0.9,*/*;q=0.5"})
        try:
            with urllib.request.urlopen(req, context=ctx, timeout=25) as r:
                self.answer(r.status, r.headers.get("Content-Type", "text/html"), r.geturl(), r.read(MAX_BYTES))
        except urllib.error.HTTPError as e:         # the site's own error page (404 and so on)
            try:
                body = e.read(MAX_BYTES)
            except Exception:
                body = b""
            self.answer(e.code, e.headers.get("Content-Type", "text/html") if e.headers else "text/html", url, body)
        except Exception as e:                      # bad certificate, DNS, timeout...
            reason = str(getattr(e, "reason", e)) or e.__class__.__name__
            reason = " ".join(reason.split())[:160]
            self.send_response(502)
            self.send_header("X-Proxy-Error", reason.encode("ascii", "replace").decode())
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()

    def answer(self, code, ctype, final_url, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("X-Final-URL", final_url)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("https proxy on 127.0.0.1:%d" % PORT, file=sys.stderr)
    srv.serve_forever()
