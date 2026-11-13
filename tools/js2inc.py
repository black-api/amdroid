#!/usr/bin/env python3
"""Turns a browser-side JavaScript file into a C string for the engine.
Usage: python3 tools/js2inc.py src/kernel/web/youtube_reader.js youtube_reader_js "comment"  > out.inc"""
import sys
src, name = sys.argv[1], sys.argv[2]
note = sys.argv[3] if len(sys.argv) > 3 else ""
js = open(src, encoding="utf-8").read()
out = "/* Generated from %s by tools/js2inc.py — edit the .js file, not this one.%s */\n" % (src.split("/")[-1], (" " + note) if note else "")
out += "static const char %s[] =\n" % name
for line in js.split("\n"):
    out += '"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\n"\n'
out += ";\n"
sys.stdout.write(out)
