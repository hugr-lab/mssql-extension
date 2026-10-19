#!/usr/bin/env python3
"""Fail if a docs build loads anything from another host.

The hugr-lab.com copy of the docs (DOCS_TARGET=ionos, website/build-ionos) is
served under a Content-Security-Policy that admits nothing from another host,
and the site's Datenschutz promises that: no image, stylesheet, font, script,
frame or fetch may leave hugr-lab.com. A plain <a href> to elsewhere is fine.

A browser would only report a violation for a page someone opens, so the
build is checked as a whole. A URL counts as foreign when it carries a scheme
(http: / https:) or is protocol-relative (//host), in any of:

  - a src / srcset (every entry) / href / data / poster attribute of a tag
    that LOADS what it names: link (except rel=canonical / alternate, which
    are metadata), script, img, source, iframe, video, audio, embed, object,
    track, input;
  - a CSS url(...) or @import, in any file.

<a href> is not looked at: a link loads nothing until it is followed.

Usage: check_docs_no_third_party.py [BUILD_DIR]   (default website/build-ionos)
"""
import pathlib
import re
import sys

LOADING_TAG = re.compile(
    r"<(link|script|img|source|iframe|video|audio|embed|object|track|input)\b[^>]*>",
    re.IGNORECASE,
)
ATTRIBUTE = re.compile(
    r"""\b(src|srcset|href|data|poster)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))""",
    re.IGNORECASE,
)
METADATA_LINK = re.compile(r"""\brel\s*=\s*["']?(canonical|alternate)\b""", re.IGNORECASE)
CSS_LOAD = re.compile(
    r"""(?:url\(\s*["']?|@import\s+(?:url\(\s*)?["']?)((?:https?:)?//[^"')\s;]+)""",
    re.IGNORECASE,
)
FOREIGN = re.compile(r"^(?:https?:)?//", re.IGNORECASE)
TEXT_SUFFIXES = {".html", ".css", ".js", ".svg", ".xml", ".json", ".txt", ".webmanifest"}


def foreign_urls(attribute, value):
    candidates = [value]
    if attribute.lower() == "srcset":
        # "a.png 1x, b.png 2x": the URL is the first word of each entry.
        candidates = [entry.strip().split()[0] for entry in value.split(",") if entry.strip()]
    return [url for url in candidates if FOREIGN.match(url.strip())]


def scan(build):
    found = set()
    for path in sorted(build.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        where = path.relative_to(build)
        if path.suffix.lower() == ".html":
            for tag in LOADING_TAG.finditer(text):
                markup = tag.group(0)
                if tag.group(1).lower() == "link" and METADATA_LINK.search(markup):
                    continue
                for attr in ATTRIBUTE.finditer(markup):
                    value = next(v for v in attr.group(2, 3, 4) if v is not None)
                    for url in foreign_urls(attr.group(1), value):
                        found.add(f"{where}: <{tag.group(1)} {attr.group(1)}> {url}")
        for css in CSS_LOAD.finditer(text):
            found.add(f"{where}: {css.group(0).split('(')[0].split()[0]} {css.group(1)}")
    return found


def main():
    build = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "website/build-ionos")
    if not build.is_dir():
        print(f"error: {build} does not exist -- build it first", file=sys.stderr)
        return 2
    found = scan(build)
    if found:
        print(f"{build} loads from another host:")
        for line in sorted(found):
            print(f"  {line}")
        return 1
    print(f"{build}: nothing loaded from another host")
    return 0


if __name__ == "__main__":
    sys.exit(main())
