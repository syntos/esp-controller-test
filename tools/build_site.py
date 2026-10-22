#!/usr/bin/env python3
"""Build site/index.html (a single self-contained page) from site/index.template.html.

The logos are embedded as base64, so the page needs no extra files. The base64 strings live
in tools/assets.b64.json; the source images are in site/assets/.
Run after editing the template:  python tools/build_site.py
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
assets = json.load(open(os.path.join(ROOT, "tools", "assets.b64.json")))
html = open(os.path.join(ROOT, "site", "index.template.html"), encoding="utf-8").read()
for token, name in {"__ICON__": "logo-icon.png", "__WORD__": "logo-text.png",
                    "__LOCKUP__": "logo-full.png", "__FAVICON__": "favicon.png"}.items():
    html = html.replace(token, assets[name])
with open(os.path.join(ROOT, "site", "index.html"), "w", encoding="utf-8", newline="\n") as f:
    f.write(html)
print("wrote site/index.html (%d KB)" % (len(html) // 1024))
