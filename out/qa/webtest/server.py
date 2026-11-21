#!/usr/bin/env python3
"""Test server for the browser, reached from the VM at http://10.0.2.2:8090/ .
Serves page.html (with cookies), and endpoints the page's scripts call."""
import http.server, json, os, sys
D = os.path.dirname(os.path.abspath(__file__))
LOG = open(os.path.join(D, "server.log"), "a")
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, f, *a): LOG.write((f % a) + "\n"); LOG.flush()
    def send(self, code, ctype, body, extra=()):
        if isinstance(body, str): body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        for k, v in extra: self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def echo(self, body=b""):
        LOG.write("echo %s cookie=%r body=%r\n" % (self.command, self.headers.get("Cookie"), body)); LOG.flush()
        self.send(200, "application/json", json.dumps({"method": self.command, "body": body.decode("utf-8", "replace"),
                  "ctype": self.headers.get("Content-Type"), "cookie": self.headers.get("Cookie")}))
    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        b = self.rfile.read(n) if n else b""
        if self.path.startswith("/form"):
            self.send(303, "text/plain", "", [("Location", "/echo?after=post")]); return
        self.echo(b)
    def do_GET(self):
        p = self.path.split("?")[0]
        if p in ("/", "/page.html"):
            self.send(200, "text/html; charset=utf-8", open(os.path.join(D, "page.html"), "rb").read(),
                      [("Set-Cookie", "srv=hello; Path=/"), ("Set-Cookie", "hid=secret; Path=/; HttpOnly"),
                       ("Set-Cookie", "old=gone; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT")])
        elif p == "/echo": self.echo()
        elif p == "/json": self.send(200, "application/json", '{"a": 1, "list": [1, 2, 3]}')
        elif p == "/missing": self.send(404, "text/plain", "nope")
        elif p == "/bytes": self.send(200, "application/octet-stream", bytes([1, 0, 0, 0, 2, 0, 0, 0]))
        elif p.endswith(".jpg"): self.send(200, "image/jpeg", open(os.path.join(D, p.lstrip("/")), "rb").read())
        else: self.send(404, "text/plain", "no such page")
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8090), H).serve_forever()
