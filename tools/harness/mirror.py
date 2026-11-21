#!/usr/bin/env python3
"""mirror.py URL DIR - saves a page for the host renderer (tools/harness/render DIR/index.html).

The page goes to DIR/index.html and its URL to DIR/url.txt. Stylesheets, scripts, images and
the url(...) resources of stylesheets go to DIR/_r/<fnv1a64 of the absolute URL>, which is
where host_main.c looks them up."""
import os, re, sys, gzip, urllib.request, urllib.parse
from concurrent.futures import ThreadPoolExecutor

UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"

def fnv(s):
    h = 1469598103934665603
    for b in s.encode():
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h

def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept-Encoding": "gzip"})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
        if r.headers.get("Content-Encoding") == "gzip":
            data = gzip.decompress(data)
        return r.geturl(), data

def main():
    url, out = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(out, "_r"), exist_ok=True)
    final, html = get(url)
    open(os.path.join(out, "index.html"), "wb").write(html)
    open(os.path.join(out, "url.txt"), "w").write(final)
    text = html.decode("utf-8", "replace")
    refs = set()
    for m in re.finditer(r'<link\b[^>]*>', text, re.I):
        tag = m.group(0)
        if re.search(r'rel=["\']?stylesheet', tag, re.I):
            h = re.search(r'href=["\']([^"\']+)', tag)
            if h: refs.add(h.group(1))
    for m in re.finditer(r'<(?:script|img|source)\b[^>]*?\s(?:src)=["\']([^"\']+)', text, re.I):
        refs.add(m.group(1))
    for m in re.finditer(r'url\(\s*["\']?([^"\')]+)', text):
        refs.add(m.group(1))

    def save(ref):
        ref = ref.replace("&amp;", "&").strip()
        if ref.startswith("data:"): return None
        absu = urllib.parse.urljoin(final, ref).split("#")[0]
        try:
            _, data = get(absu)
        except Exception as e:
            print("  miss", absu, e)
            return None
        open(os.path.join(out, "_r", fnv(absu)), "wb").write(data)
        return absu, data

    with ThreadPoolExecutor(8) as ex:
        got = [g for g in ex.map(save, refs) if g]
    # one level of stylesheet url(...) resources and @imports
    more = set()
    for absu, data in got:
        if absu.split("?")[0].endswith(".css") or b"{" in data[:200]:
            for m in re.finditer(rb'url\(\s*["\']?([^"\')]+)', data):
                r = m.group(1).decode("utf-8", "replace")
                if not r.startswith("data:"): more.add(urllib.parse.urljoin(absu, r))
    with ThreadPoolExecutor(8) as ex:
        list(ex.map(save, more))
    print("saved", final, "+", len(got) + len(more), "resources")

main()
