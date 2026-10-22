#!/usr/bin/env python3
"""Guard the Home Assistant entity names.

Home Assistant builds entity IDs from these names. Renaming an input, output or any other
entity breaks users' automations and dashboards, so CI compares the names in the generated
configs with the lists in compat/.

    python tools/check_entities.py            # check (used by CI)
    python tools/check_entities.py --update   # accept an intentional change (mention it in the release notes!)
"""
import os
import sys

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VARIANTS = ["ethernet", "wifi"]
DOMAINS = ["binary_sensor", "light", "button", "switch", "sensor", "text_sensor", "output"]


class Loader(yaml.SafeLoader):
    pass


def collect(variant):
    with open(os.path.join(ROOT, "esp-controller-%s.yaml" % variant), encoding="utf-8") as f:
        doc = yaml.load(f, Loader=Loader)
    subs = doc.get("substitutions", {})

    def sub(s):
        for k, v in subs.items():
            s = s.replace("${%s}" % k, str(v))
        return s

    names = set()
    for domain in DOMAINS:
        if domain == "output":
            continue  # outputs are internal, not exposed to Home Assistant
        for item in doc.get(domain, []) or []:
            if "name" in item:
                names.add("%s: %s" % (domain, sub(str(item["name"]))))
            # platforms like ethernet_info / wifi_info define entities one level deeper
            for key, val in item.items():
                if isinstance(val, dict) and "name" in val:
                    names.add("%s: %s" % (domain, sub(str(val["name"]))))
    return sorted(names)


def main():
    update = "--update" in sys.argv
    ok = True
    for v in VARIANTS:
        current = collect(v)
        path = os.path.join(ROOT, "compat", "%s.txt" % v)
        if update:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(current) + "\n")
            print("updated", os.path.relpath(path, ROOT), "(%d entities)" % len(current))
            continue
        with open(path, encoding="utf-8") as f:
            expected = [l.rstrip("\n") for l in f if l.strip()]
        removed = sorted(set(expected) - set(current))
        added = sorted(set(current) - set(expected))
        if removed or added:
            ok = False
            print("Entity names changed in the %s variant:" % v)
            for r in removed:
                print("  - removed/renamed:", r)
            for a in added:
                print("  + added:", a)
        else:
            print("%s: %d entities OK" % (v, len(current)))
    if not ok:
        print("\nIf this is intentional, run: python tools/check_entities.py --update  (and mention it in the release notes)")
        sys.exit(1)


if __name__ == "__main__":
    main()
