#!/usr/bin/env python3
"""Fail if a docs page links to another page by absolute site path.

Docusaurus resolves `](/writing/copy/)` against `baseUrl`, which always
points at the *latest released* version. A page under docs/ publishes as
/next/, so an absolute link there sends the reader out of the version they
are reading and into the released snapshot -- and a page inside
versioned_docs/version-0.2.3/ sends them forward to 0.2.4. Version-relative
markdown links (`../writing/copy.md`) resolve within the version and are
checked by Docusaurus itself.

This exists because the same defect was swept by hand twice and both sweeps
missed sections nobody thought to list (connection/, performance/). The rule
is mechanical, so a machine should enforce it.
"""
import os
import re
import sys
import glob

PATTERN = re.compile(r"\]\((/(?!/)[^)#\s]*)(#[^)\s]*)?\)")


def doc_roots():
    roots = []
    if os.path.isdir("website/docs"):
        roots.append("website/docs")
    roots.extend(sorted(glob.glob("website/versioned_docs/version-*")))
    return roots


def resolve(root, path):
    """Return the file an absolute link would name inside `root`, or None."""
    stem = path.strip("/")
    if not stem:
        return None
    for cand in (os.path.join(root, stem + ".md"),
                 os.path.join(root, stem, "index.md")):
        if os.path.exists(cand):
            return cand
    return None


def main():
    roots = doc_roots()
    if not roots:
        print("no docs roots found under website/ -- nothing to check")
        return 0

    bad = []
    for root in roots:
        for dirpath, _, files in os.walk(root):
            for fn in files:
                if not fn.endswith(".md"):
                    continue
                p = os.path.join(dirpath, fn)
                for lineno, line in enumerate(open(p), 1):
                    for m in PATTERN.finditer(line):
                        target = resolve(root, m.group(1))
                        if target is None:
                            # Not a page in this version: an asset path, an
                            # external-ish route, or a genuine typo that
                            # Docusaurus' own onBrokenLinks will catch.
                            continue
                        rel = os.path.relpath(target, dirpath)
                        if not rel.startswith("."):
                            rel = "./" + rel
                        bad.append((p, lineno, m.group(0),
                                    "](%s%s)" % (rel, m.group(2) or "")))

    if not bad:
        print("checked %d docs root(s): no absolute internal links" % len(roots))
        return 0

    print("Absolute internal doc links found (%d). Each one escapes its own "
          "version:\n" % len(bad))
    for p, lineno, found, suggested in bad:
        print("  %s:%d\n      %s\n      use %s" % (p, lineno, found, suggested))
    print("\nUse version-relative markdown links so a page links within its "
          "own version.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
