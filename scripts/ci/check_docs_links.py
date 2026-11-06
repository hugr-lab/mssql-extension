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
import tempfile

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


def version_relative(target, dirpath, anchor):
    """The markdown link that reaches `target` from `dirpath`, anchor kept."""
    rel = os.path.relpath(target, dirpath)
    if not rel.startswith("."):
        rel = "./" + rel
    return "](%s%s)" % (rel, anchor or "")


def links_in_file(root, path):
    """[(path, lineno, found, suggested)] for one markdown file."""
    dirpath = os.path.dirname(path)
    found = []
    for lineno, line in enumerate(open(path), 1):
        for m in PATTERN.finditer(line):
            target = resolve(root, m.group(1))
            if target is None:
                # Not a page in this version: an asset path, an external-ish
                # route, or a genuine typo that Docusaurus' own
                # onBrokenLinks will catch.
                continue
            found.append((path, lineno, m.group(0),
                          version_relative(target, dirpath, m.group(2))))
    return found


def absolute_links(roots):
    """Every absolute internal link under `roots`, in walk order."""
    bad = []
    for root in roots:
        for dirpath, _, files in os.walk(root):
            for fn in files:
                if not fn.endswith(".md"):
                    continue
                bad.extend(links_in_file(root, os.path.join(dirpath, fn)))
    return bad


def report(roots, bad):
    """Print the verdict. -> the process exit status."""
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


# ---------------------------------------------------------------- self-test

#: (label, the link as written, is it an absolute internal link to flag)
#
# The rule reads simply and is easy to get subtly wrong, so each case is here:
# what must be flagged, and -- more important for a CI gate -- what must not,
# since a false positive here blocks every docs PR.
LINK_TESTS = [
    ("an absolute link to a page in this version", "](/writing/copy/)", True),
    ("the same without the trailing slash", "](/writing/copy)", True),
    ("an absolute link with an anchor", "](/writing/copy/#bulk)", True),
    ("an absolute link to a section index", "](/connection/)", True),
    ("a version-relative link", "](../writing/copy.md)", False),
    ("a same-directory relative link", "](./copy.md)", False),
    ("an absolute path that is NOT a page in this version", "](/img/logo.png)", False),
    ("a protocol-relative URL is not a site path", "](//example.com/x)", False),
    ("an external URL", "](https://example.com/writing/copy/)", False),
    ("a bare root link", "](/)", False),
]


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(text)


def _line(ok, label, detail):
    print("  [%s] %-58s -> %s" % ("ok" if ok else "FAIL", label, detail))
    return 0 if ok else 1


def _fixture_root(d):
    """A docs version holding the pages an absolute link could name.

    Both forms resolve() accepts -- `<stem>.md` and `<stem>/index.md` -- plus a
    non-page asset, since an absolute path to one must NOT be flagged.
    """
    root = os.path.join(d, "version-0.0.0")
    _write(os.path.join(root, "writing", "copy.md"), "x\n")
    _write(os.path.join(root, "connection", "index.md"), "x\n")
    _write(os.path.join(root, "img", "logo.png"), "x\n")
    return root


def _check_rule(root, page):
    print("the absolute-internal-link rule:")
    failures = 0
    for label, link, want in LINK_TESTS:
        _write(page, "see [copy]%s for more\n" % link)
        got = links_in_file(root, page)
        failures += _line(bool(got) == want, label,
                          ("suggests %s" % got[0][3]) if got else "clean")
    return failures


def _check_suggestion(root, page):
    """The suggested link must resolve to THE LINKED PAGE.

    A wrong suggestion is worse than none: it sends the author to a second
    broken link while telling them it is the fix. The first cut of this check
    only asked whether the suggested path EXISTS, which a regression pointing
    at the page itself (`./start.md`) or at any other real file would have
    satisfied -- so it could not fail, and its label was a claim it did not
    test (roborev on 4785932).
    """
    failures = 0
    for link, want in (("/writing/copy/", os.path.join(root, "writing", "copy.md")),
                       ("/connection/", os.path.join(root, "connection", "index.md"))):
        _write(page, "see [x](%s) for more\n" % link)
        found = links_in_file(root, page)
        rel = found[0][3][2:-1] if found else ""
        target = os.path.normpath(os.path.join(os.path.dirname(page), rel))
        failures += _line(bool(found) and target == os.path.normpath(want),
                          "%s is suggested as the page itself, not merely a real file" % link,
                          rel or "NOTHING")
    return failures


def _check_clean(root, page):
    _write(page, "see [copy](../writing/copy.md) for more\n")
    return _line(absolute_links([root]) == [],
                 "a clean docs tree reports nothing", "clean")


def self_test():
    """Check the rule against its own fixtures. -> failure count."""
    with tempfile.TemporaryDirectory() as d:
        root = _fixture_root(d)
        page = os.path.join(root, "guide", "start.md")
        failures = _check_rule(root, page)
        failures += _check_suggestion(root, page)
        failures += _check_clean(root, page)
    print("\n%s: self-test, %d failure(s)" % ("FAIL" if failures else "PASS", failures))
    return failures


def main():
    if "--self-test" in sys.argv[1:]:
        return 1 if self_test() else 0
    roots = doc_roots()
    if not roots:
        print("no docs roots found under website/ -- nothing to check")
        return 0
    return report(roots, absolute_links(roots))


if __name__ == "__main__":
    sys.exit(main())
