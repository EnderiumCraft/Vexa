#!/usr/bin/env python3
"""Makes pkg's packages: each app bundle zipped, and index.conf listing them
(with each zip's size and SHA-256), in OUT.

    make-packages.py OUT ID:VERSION:CATEGORY:SUMMARY:BUNDLE ...
"""
import hashlib
import os
import sys
import zipfile


def read_info(bundle):
    info = {}
    with open(os.path.join(bundle, "Contents", "Info.conf")) as f:
        for line in f:
            key, _, value = line.strip().partition("=")
            info[key] = value
    return info


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    index = []
    for spec in sys.argv[2:]:
        package, version, category, summary, bundle = spec.split(":", 4)
        bundle = bundle.rstrip("/")
        name = os.path.basename(bundle)
        archive = os.path.join(out, name + ".zip")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            for root, dirs, files in os.walk(bundle):
                dirs.sort()
                for file in sorted(files):
                    if file.startswith("."):
                        continue
                    path = os.path.join(root, file)
                    z.write(path, os.path.join(name, os.path.relpath(path, bundle)))
        with open(archive, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        info = read_info(bundle)
        index.append("[%s]\nname=%s\nversion=%s\ncategory=%s\nsummary=%s\nbundle=%s\n"
                     "file=%s\nsize=%d\nsha256=%s\n" % (
                         package, info.get("name", name), version, category, summary, name,
                         name + ".zip", os.path.getsize(archive), digest))
    with open(os.path.join(out, "index.conf"), "w") as f:
        f.write("# Vexa's packages, for pkg and the Software app.\n\n" + "\n".join(index))


main()
