#!/usr/bin/env python3
"""Generate the two ready-to-use ESPHome configs from the parts in src/.

    python tools/build_yaml.py                  # write esp-controller-ethernet.yaml / -wifi.yaml
    python tools/build_yaml.py --check          # fail if the committed files are out of date
    python tools/build_yaml.py --local --out build
                                                # CI build: use the components in this checkout

Settings (repo, version, ref, factory key) live in project.json.
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VARIANTS = ["ethernet", "wifi"]
KEY_RE = re.compile(r"^([A-Za-z_][A-Za-z_0-9]*):")


def split_blocks(text):
    """Split a YAML file into [(key, text)] top-level blocks, keeping comments above a key with it."""
    blocks, pending, current = [], [], None
    for line in text.split("\n"):
        m = KEY_RE.match(line)
        if m:
            if current:
                blocks.append(current)
            current = [m.group(1), "\n".join(pending + [line])]
            pending = []
        elif current is None:
            pending.append(line)
        elif line.startswith("#") or line.strip() == "":
            # comment/blank lines between blocks belong to the next block
            if line.strip() == "" and not pending:
                current[1] += "\n"
            else:
                pending.append(line)
        else:
            if pending:
                current[1] += "\n" + "\n".join(pending)
                pending = []
            current[1] += "\n" + line
    if current:
        blocks.append(current)
    return [(k, t.rstrip() + "\n") for k, t in blocks]


def substitution_lines(blocks):
    for k, t in blocks:
        if k == "substitutions":
            lines = t.split("\n")
            start = next(i for i, l in enumerate(lines) if l.startswith("substitutions:"))
            return [l for l in lines[start + 1:] if l.startswith(" ") and l.strip()]
    return []


def load(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def generate(variant, cfg, local, out_dir):
    common = split_blocks(load("src/common.yaml"))
    network = split_blocks(load(f"src/network-{variant}.yaml"))
    version = cfg["version"]
    ref = cfg["ref"] if cfg["ref"] != "auto" else "v" + version
    refresh = "1d" if re.match(r"^v?\d", ref) else "always"

    subs = ["  project_version: \"%s\"" % version,
            "  factory_api_key: \"%s\"" % cfg["factory_api_key"],
            "  editor_url: \"https://%s.github.io/%s/\"" % tuple(cfg["repo"].split("/"))]
    subs += substitution_lines(network) + substitution_lines(common)

    if local:
        rel = os.path.relpath(os.path.join(ROOT, "components"), os.path.join(ROOT, out_dir)).replace(os.sep, "/")
        ext = "external_components:\n  - source:\n      type: local\n      path: %s\n    components: [fallback_engine]\n" % rel
    else:
        ext = ("external_components:\n  - source:\n      type: git\n      url: https://github.com/%s\n"
               "      ref: %s\n    refresh: %s\n    components: [fallback_engine]\n") % (cfg["repo"], ref, refresh)

    dash = ("# Used by ESPHome Builder when a user clicks \"Take Control\": it creates a small personal\n"
            "# config with its own API key that points to this file.\n"
            "dashboard_import:\n  package_import_url: github://%s/esp-controller-%s.yaml@%s\n"
            "  import_full_config: false\n") % (cfg["repo"], variant, ref)

    seen = {}
    body = []
    for source, blocks in (("network", network), ("common", common)):
        for k, t in blocks:
            if k == "substitutions":
                continue
            if k in seen:
                sys.exit("Top-level key '%s' is defined in both src/%s and src/%s" % (k, seen[k], source))
            seen[k] = source
            body.append(t)

    header = ("# GENERATED FILE - do not edit by hand.\n"
              "# Source: src/common.yaml + src/network-%s.yaml. Rebuild with: python tools/build_yaml.py\n"
              "#\n"
              "# Logi.casa ESP Home Controller, %s variant, version %s.\n"
              "# Contains no private secrets. Your own API key is created when you take control in ESPHome Builder.\n\n") % (
                  variant, variant, version)
    out = header + "substitutions:\n" + "\n".join(subs) + "\n\n" + dash + "\n" + ext + "\n" + "\n".join(body)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--local", action="store_true", help="use the components of this checkout")
    ap.add_argument("--out", default=".")
    args = ap.parse_args()
    with open(os.path.join(ROOT, "project.json"), encoding="utf-8") as f:
        cfg = json.load(f)
    bad = False
    for v in VARIANTS:
        text = generate(v, cfg, args.local, args.out)
        path = os.path.join(ROOT, args.out, "esp-controller-%s.yaml" % v)
        if args.check:
            current = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
            if current != text:
                print("OUT OF DATE: %s (run python tools/build_yaml.py)" % os.path.relpath(path, ROOT))
                bad = True
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            print("wrote", os.path.relpath(path, ROOT))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
