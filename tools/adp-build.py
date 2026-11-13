#!/usr/bin/env python3
"""adp-build - the Am-Droid app build tool.

Packages an app project directory into a single ".adp" (Am-Droid Package) file.

A project directory contains:
  - app.manifest : key=value lines (id, name, version, author, icon, desc,
                   perms, entry). See examples/apps/hello/app.manifest.
  - one or more program/asset files referenced by the app (e.g. MAIN.FAS).

The .adp container (ADP1) is:

  ADP1\\n
  <key>=<value>\\n            (the manifest header, one key per line)
  ...
  %%FILE <NAME> <LEN>\\n       (one section per bundled file)
  <LEN bytes of raw data>\\n
  ...
  %%END\\n

File names must be valid FAT32 8.3 names (<=8 chars, <=3 ext) and the app id
must be a valid 8.3 directory name, because that is how they land on the disk.

Usage:
  tools/adp-build.py <project-dir> [-o out.adp]
"""
import argparse
import os
import sys

REQUIRED = ("id", "name", "version", "entry", "requires")
KNOWN = ("id", "name", "version", "author", "icon", "desc", "perms", "requires", "entry")
# Permissions the platform understands; the ones marked dangerous are shown in
# red at install time and must be declared when the code uses them.
PERMS_KNOWN = ("framebuffer", "input", "storage", "network", "system")
PERMS_DANGEROUS = ("storage", "network", "system")


def fail(msg):
    sys.stderr.write("adp-build: " + msg + "\n")
    sys.exit(1)


def valid_83(name):
    stem, _, ext = name.partition(".")
    if not stem or len(stem) > 8 or len(ext) > 3:
        return False
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
    return all(c.upper() in ok for c in stem) and all(c.upper() in ok for c in ext)


def read_manifest(path):
    meta = {}
    with open(path, "r", encoding="utf-8") as f:
        for ln, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                fail(f"{path}:{ln}: expected key=value")
            k, _, v = line.partition("=")
            meta[k.strip()] = v.strip()
    for k in REQUIRED:
        if k not in meta:
            fail(f"{path}: missing required key '{k}'")
    for k in meta:
        if k not in KNOWN:
            sys.stderr.write(f"adp-build: warning: unknown manifest key '{k}'\n")
    if not valid_83(meta["id"] + ".x"):
        fail(f"id '{meta['id']}' is not a valid 8.3 name (<=8 chars A-Z0-9_-)")
    if not valid_83(meta["entry"]):
        fail(f"entry '{meta['entry']}' is not a valid 8.3 file name")
    req = meta["requires"].lower()
    if "any" not in req and not req.startswith("amdroid"):
        fail(f"requires '{meta['requires']}' must look like 'amdroid1+', 'amdroid2.5' or 'amdroid.any'")
    for p in (x.strip() for x in meta.get("perms", "").split(",") if x.strip()):
        if p not in PERMS_KNOWN:
            sys.stderr.write(f"adp-build: warning: unknown permission '{p}'\n")
    meta["id"] = meta["id"].upper()
    meta["entry"] = meta["entry"].upper()
    return meta


def collect_files(proj):
    """Every file in the project except the manifest is bundled, 8.3-upcased."""
    files = []
    for fn in sorted(os.listdir(proj)):
        p = os.path.join(proj, fn)
        if not os.path.isfile(p) or fn == "app.manifest":
            continue
        name = fn.upper()
        if not valid_83(name):
            fail(f"file '{fn}' is not a valid 8.3 name")
        with open(p, "rb") as f:
            files.append((name, f.read()))
    return files


def build(proj, out):
    meta = read_manifest(os.path.join(proj, "app.manifest"))
    files = collect_files(proj)
    names = {n for n, _ in files}
    if meta["entry"] not in names:
        fail(f"entry '{meta['entry']}' is not one of the project files {sorted(names)}")

    blob = bytearray(b"ADP1\n")
    for k in KNOWN:
        if k in meta:
            blob += f"{k}={meta[k]}\n".encode("utf-8")
    for name, data in files:
        blob += f"%%FILE {name} {len(data)}\n".encode("utf-8")
        blob += data
        blob += b"\n"
    blob += b"%%END\n"

    with open(out, "wb") as f:
        f.write(blob)
    print(f"built {out}  ({meta['id']} v{meta['version']}, "
          f"{len(files)} file(s), {len(blob)} bytes)")


def main():
    ap = argparse.ArgumentParser(description="Package an Am-Droid app into a .adp")
    ap.add_argument("project", help="app project directory (holds app.manifest)")
    ap.add_argument("-o", "--out", help="output .adp path")
    args = ap.parse_args()
    proj = args.project.rstrip("/")
    if not os.path.isdir(proj):
        fail(f"'{proj}' is not a directory")
    out = args.out
    if not out:
        meta = read_manifest(os.path.join(proj, "app.manifest"))
        out = meta["id"].upper() + ".ADP"
    build(proj, out)


if __name__ == "__main__":
    main()
