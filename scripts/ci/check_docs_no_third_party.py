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
import tempfile

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


def html_loads(text, where):
    """Foreign URLs that HTML in one file LOADS (not merely links to)."""
    found = set()
    for tag in LOADING_TAG.finditer(text):
        markup = tag.group(0)
        if tag.group(1).lower() == "link" and METADATA_LINK.search(markup):
            # rel=canonical / alternate are metadata: the browser loads nothing.
            continue
        for attr in ATTRIBUTE.finditer(markup):
            value = next(v for v in attr.group(2, 3, 4) if v is not None)
            for url in foreign_urls(attr.group(1), value):
                found.add(f"{where}: <{tag.group(1)} {attr.group(1)}> {url}")
    return found


def css_loads(text, where):
    """Foreign URLs a CSS `url(...)` or `@import` loads, in any file type."""
    return {f"{where}: {m.group(0).split('(')[0].split()[0]} {m.group(1)}"
            for m in CSS_LOAD.finditer(text)}


def scan(build):
    found = set()
    for path in sorted(build.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        where = path.relative_to(build)
        if path.suffix.lower() == ".html":
            found |= html_loads(text, where)
        found |= css_loads(text, where)
    return found


# ---------------------------------------------------------------- self-test

#: (label, markup, must it be reported)
#
# Both directions, because a false positive here blocks every docs deploy while
# a false negative silently breaks the CSP and the Datenschutz promise. The
# must-NOT cases are the ones worth having: each is a shape that looks foreign
# and loads nothing, or loads nothing from elsewhere.
HTML_TESTS = [
    ("an absolute foreign image", '<img src="https://cdn.example.com/x.png">', True),
    ("a protocol-relative script", '<script src="//cdn.example.com/x.js"></script>', True),
    ("a foreign stylesheet", '<link rel="stylesheet" href="https://cdn.example.com/a.css">', True),
    ("an unquoted foreign iframe src", "<iframe src=https://example.com/f></iframe>", True),
    ("a foreign entry anywhere in a srcset",
     '<img srcset="/img/a.png 1x, https://cdn.example.com/b.png 2x">', True),
    ("a foreign video poster", '<video poster="//cdn.example.com/p.jpg"></video>', True),
    ("a foreign object data", '<object data="https://example.com/o.svg"></object>', True),
    # Must NOT be reported.
    ("an anchor to another site -- a link loads nothing",
     '<a href="https://example.com/page">x</a>', False),
    ("rel=canonical is metadata", '<link rel="canonical" href="https://hugr-lab.com/x/">', False),
    ("rel=alternate is metadata",
     '<link rel="alternate" href="https://hugr-lab.com/rss.xml">', False),
    ("a root-relative image", '<img src="/img/logo.png">', False),
    ("a same-directory image", '<img src="./logo.png">', False),
    ("an inline data URI", '<img src="data:image/gif;base64,R0lGOD">', False),
    ("a mailto anchor", '<a href="mailto:x@hugr-lab.com">x</a>', False),
]

#: (label, stylesheet text, must it be reported)
CSS_TESTS = [
    ("a foreign url()", "body { background: url(https://cdn.example.com/b.png); }", True),
    ("a quoted foreign url()", 'body { background: url("//cdn.example.com/b.png"); }', True),
    ("a foreign @import", '@import "https://fonts.example.com/f.css";', True),
    ("a foreign @import url()", "@import url(//fonts.example.com/f.css);", True),
    ("a root-relative url()", "body { background: url(/img/b.png); }", False),
    ("a data: url()", "body { background: url(data:image/gif;base64,R0lGOD); }", False),
    ("a relative font url()", 'src: url("../fonts/x.woff2");', False),
]


def _line(ok, label, detail):
    print("  [%s] %-56s -> %s" % ("ok" if ok else "FAIL", label, detail))
    return 0 if ok else 1


def _check_group(heading, fixtures, judge):
    print(heading)
    failures = 0
    for label, text, want in fixtures:
        got = judge(text)
        failures += _line(bool(got) == want,
                          label, sorted(got)[0][:46] if got else "clean")
    return failures


def _check_whole_build():
    """`scan()` over a directory, which is what CI actually calls.

    The per-snippet fixtures above exercise the two matchers; this proves the
    walk reaches both file types, skips what it should and reports the path.
    """
    print("\nscan() over a build directory:")
    failures = 0
    with tempfile.TemporaryDirectory() as d:
        build = pathlib.Path(d)
        (build / "sub").mkdir()
        (build / "index.html").write_text('<img src="https://cdn.example.com/x.png">\n')
        (build / "sub" / "a.css").write_text("body { background: url(//cdn.example.com/b.png); }\n")
        # Not a text suffix: never read, so a foreign URL inside is invisible.
        # The content has to be something css_loads WOULD flag, since that
        # matcher runs on every text file: an <img src> here could not fail the
        # check even if the suffix filter broke, because html_loads only runs
        # on .html (roborev on 8bbfff4).
        (build / "sub" / "blob.bin").write_text(
            "body { background: url(https://cdn.example.com/no.png); }\n")
        found = scan(build)
        failures += _line(len(found) == 2, "one finding per file, the .bin skipped",
                          "%d finding(s)" % len(found))
        failures += _line(any(f.startswith("index.html:") for f in found),
                          "the HTML finding names its path relative to the build", "index.html")
        failures += _line(any(f.startswith("sub/a.css:") for f in found),
                          "the CSS finding names its nested path", "sub/a.css")

        # A clean build must pass, so the gate can ever be green.
        (build / "index.html").write_text('<a href="https://example.com">x</a>\n')
        (build / "sub" / "a.css").write_text("body { background: url(/img/b.png); }\n")
        failures += _line(scan(build) == set(), "a clean build reports nothing", "clean")
    return failures


def self_test():
    failures = _check_group("HTML that loads, vs HTML that only links:",
                            HTML_TESTS, lambda t: html_loads(t, "p.html"))
    failures += _check_group("\nCSS url() and @import:",
                             CSS_TESTS, lambda t: css_loads(t, "p.css"))
    failures += _check_whole_build()
    print("\n%s: self-test, %d failure(s)" % ("FAIL" if failures else "PASS", failures))
    return failures


def main():
    if "--self-test" in sys.argv[1:]:
        return 1 if self_test() else 0
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
