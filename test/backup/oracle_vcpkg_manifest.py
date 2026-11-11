#!/usr/bin/env python3
"""Spec 085 PR 2a: the vcpkg manifest of the backup oracle build, written to OUT_DIR/vcpkg.json.

The repository's vcpkg.json with curl added: httpfs, linked into that build, needs curl (its own
vcpkg.json lists it), and without it in the manifest find_package(CURL) takes the system libcurl,
which brings the system's OpenSSL beside our static one. Derived from vcpkg.json, never kept as a
second copy, so its baseline, overrides and dependencies are always the repository's. Not the
repository's manifest: curl has no place in the shipped extension.

    python3 -I test/backup/oracle_vcpkg_manifest.py build/backup-oracle-vcpkg
"""
import json
import pathlib
import sys

CURL = {"name": "curl", "default-features": False, "features": ["ssl"]}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    repo = pathlib.Path(__file__).resolve().parents[2]
    manifest = json.loads((repo / "vcpkg.json").read_text())
    names = [d if isinstance(d, str) else d["name"] for d in manifest["dependencies"]]
    if "curl" not in names:
        manifest["dependencies"].append(CURL)
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    (out / "vcpkg.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
