#!/usr/bin/env python3
"""Update README badges, version and the download-metrics chart.

The data is fetched and aggregated *with DuckDB itself* (dogfooding) — the same
`read_json` queries the DuckDB Community Extensions metrics page documents:

  - https://community-extensions.duckdb.org/downloads-last-week.json
        rolling "downloads in the last 7 days" snapshot, keyed by extension name.
  - https://community-extensions.duckdb.org/download-stats-weekly/{year}/{week}.json
        the same snapshot archived once per ISO week -> our time series.

DuckDB (httpfs + read_json) does the fetch/aggregation; this script renders the
result into a self-contained SVG line chart (GitHub sanitizes README HTML, so no
JS/WASM there) and can rewrite the managed blocks in README.md delimited by
  <!-- METRICS-BADGES:START --> ... <!-- METRICS-BADGES:END -->
  <!-- METRICS-CHART:START --> ... <!-- METRICS-CHART:END -->

Two modes:

  --svg-only PATH   render the chart to PATH and touch nothing else. This is
                    what the weekly workflow uses: the SVG is published to
                    GitHub Pages, so the chart stays current with NO commit to
                    the repository. `main` is a protected branch — a scheduled
                    job cannot push to it, and used to fail every Monday with
                    GH006 for exactly that reason.

  (no arguments)    additionally rewrite the README blocks. Run by hand when the
                    badge row or the chart section itself changes; the numbers in
                    them are live (a dynamic shields badge, a Pages-hosted image)
                    and do not need a weekly commit.

See https://duckdb.org/community_extensions/download_metrics
"""

import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request

EXTENSION = "mssql"
REPO = os.environ.get("GITHUB_REPOSITORY", "hugr-lab/mssql-extension")
START_YEAR = 2026  # weekly archive begins 2026-W01 (week of 2026-01-04)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
README = os.path.join(ROOT, "README.md")
SVG = os.path.join(ROOT, "docs", "assets", "download-metrics.svg")
DESCRIPTION = os.path.join(ROOT, "description.yml")

LAST_WEEK_URL = "https://community-extensions.duckdb.org/downloads-last-week.json"
WEEKLY_URL = "https://community-extensions.duckdb.org/download-stats-weekly/{year}/{week}.json"

# Two managed regions: a shields badge row above the H1 (h3-duckdb convention)
# and the download chart under the lead paragraph.
BADGES_BEGIN = "<!-- METRICS-BADGES:START -->"
BADGES_END = "<!-- METRICS-BADGES:END -->"
CHART_BEGIN = "<!-- METRICS-CHART:START -->"
CHART_END = "<!-- METRICS-CHART:END -->"

DUCKDB_MIN_VERSION = "v1.4.1"  # minimum supported DuckDB (see README Prerequisites)
LICENSE_NAME = "MIT"

UA = {"User-Agent": "mssql-extension-metrics-bot/1.0"}


# --------------------------------------------------------------------------- #
# DuckDB-backed fetching / aggregation
# --------------------------------------------------------------------------- #
def find_duckdb():
    """Resolve a DuckDB CLI: $DUCKDB_BIN, the extension's own build, or PATH."""
    candidates = [
        os.environ.get("DUCKDB_BIN"),
        os.path.join(ROOT, "build", "release", "duckdb"),
        os.path.join(ROOT, "build", "debug", "duckdb"),
        shutil.which("duckdb"),
    ]
    for c in candidates:
        if c and os.path.exists(c):
            return c
    if candidates[-1]:  # shutil.which returned a name on PATH
        return candidates[-1]
    raise RuntimeError(
        "no DuckDB CLI found; set DUCKDB_BIN or install duckdb on PATH"
    )


DUCKDB = None


def run_duckdb(sql):
    """Run SQL via the DuckDB CLI in JSON mode; return parsed rows (list)."""
    global DUCKDB
    if DUCKDB is None:
        DUCKDB = find_duckdb()
    proc = subprocess.run(
        [DUCKDB, "-json", "-c", sql],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise DuckDBError(proc.stderr.strip() or "duckdb failed")
    out = proc.stdout.strip()
    if not out:
        return []
    return json.loads(out)


class DuckDBError(RuntimeError):
    pass


def url_literal(urls):
    quoted = ", ".join("'%s'" % u.replace("'", "''") for u in urls)
    return "[" + quoted + "]"


def fetch_total_last_week():
    rows = run_duckdb(
        "INSTALL httpfs; LOAD httpfs;\n"
        "SELECT %s AS total FROM read_json('%s');" % (EXTENSION, LAST_WEEK_URL)
    )
    if not rows or rows[0].get("total") is None:
        return 0
    return int(rows[0]["total"])


def fetch_series():
    """Read the weekly archive with DuckDB; one (date, downloads) per week.

    The not-yet-published current ISO week 404s and would abort read_json, so we
    drop trailing weeks and retry until the read succeeds.
    """
    cur_year, cur_week = iso_week_now()
    urls = []
    for year in range(START_YEAR, cur_year + 1):
        last_week = cur_week if year == cur_year else 53
        for week in range(1, last_week + 1):
            urls.append(WEEKLY_URL.format(year=year, week=week))

    while urls:
        sql = (
            "INSTALL httpfs; LOAD httpfs;\n"
            "SELECT CAST(_last_update AS DATE)::VARCHAR AS d, %s AS downloads\n"
            "FROM read_json(%s, union_by_name = true)\n"
            "WHERE %s IS NOT NULL\n"
            "ORDER BY _last_update;" % (EXTENSION, url_literal(urls), EXTENSION)
        )
        try:
            rows = run_duckdb(sql)
        except DuckDBError as exc:
            if "404" in str(exc) and len(urls) > 1:
                dropped = urls.pop()
                print("  (skipping unpublished week: %s)" % dropped.rsplit("/", 1)[-1])
                continue
            raise
        return [(r["d"], int(r["downloads"])) for r in rows]
    return []


def latest_release():
    """Latest published version: GitHub release tag, else description.yml."""
    url = "https://api.github.com/repos/%s/releases/latest" % REPO
    headers = dict(UA)
    headers["Accept"] = "application/vnd.github+json"
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            tag = json.loads(resp.read().decode("utf-8")).get("tag_name")
            if tag:
                return tag.lstrip("v")
    except Exception:
        pass
    # fallback: description.yml `version: "x.y.z"`
    try:
        with open(DESCRIPTION, encoding="utf-8") as fh:
            for line in fh:
                m = re.match(r'\s*version:\s*"?([0-9][^"\s]*)"?', line)
                if m:
                    return m.group(1)
    except OSError:
        pass
    return "unknown"


def iso_week_now():
    today = datetime.date.today()
    y, w, _ = today.isocalendar()
    return y, w


# --------------------------------------------------------------------------- #
# SVG chart
# --------------------------------------------------------------------------- #
def esc(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Plot:
    """The chart's box and its two scales. Pure geometry: emits no SVG.

    Shared by the element builders below so the grid, the series, the dots and
    the labels cannot drift apart -- they all ask the same x() and y().
    """

    W, H = 760, 280
    ml, mr, mt, mb = 56, 24, 44, 48

    def __init__(self, points):
        self.pw = self.W - self.ml - self.mr
        self.ph = self.H - self.mt - self.mb
        self.n = len(points)
        self.vmax_nice = nice_ceil(max([v for _, v in points] or [0]) or 1)

    def x(self, i):
        # One point has no span to spread over, so it sits in the middle
        # rather than at the left edge (and never divides by n - 1 = 0).
        if self.n <= 1:
            return self.ml + self.pw / 2
        return self.ml + self.pw * i / (self.n - 1)

    def y(self, v):
        return self.mt + self.ph * (1 - v / self.vmax_nice)

    @property
    def baseline(self):
        return self.mt + self.ph


def grid_lines(plot):
    return ['<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" '
            'stroke="#e3e8ee" stroke-width="1"/>'
            % (plot.ml, plot.y(plot.vmax_nice * g / 4),
               plot.ml + plot.pw, plot.y(plot.vmax_nice * g / 4))
            for g in range(5)]


def y_axis_labels(plot):
    return ['<text x="%.1f" y="%.1f" font-size="11" fill="#8a94a6" '
            'text-anchor="end" dominant-baseline="middle">%s</text>'
            % (plot.ml - 8, plot.y(plot.vmax_nice * g / 4),
               fmt_short(plot.vmax_nice * g / 4))
            for g in range(5)]


def series_points(plot, points):
    """(polyline points, polygon points) -- the line and the filled area."""
    line_pts = " ".join("%.1f,%.1f" % (plot.x(i), plot.y(v))
                        for i, (_, v) in enumerate(points))
    area_pts = (
        "%.1f,%.1f " % (plot.x(0), plot.baseline)
        + line_pts
        + " %.1f,%.1f" % (plot.x(plot.n - 1), plot.baseline)
    ) if plot.n else ""
    return line_pts, area_pts


def dot_markers(plot, points):
    return ['<circle cx="%.1f" cy="%.1f" r="2.6" fill="#fff" '
            'stroke="#f6c344" stroke-width="2"/>' % (plot.x(i), plot.y(v))
            for i, (_, v) in enumerate(points)]


def x_axis_labels(plot, points):
    """Thinned to ~8 ticks, and the last sample always labelled."""
    step = max(1, plot.n // 8)
    return ['<text x="%.1f" y="%.1f" font-size="10" fill="#8a94a6" '
            'text-anchor="middle">%s</text>'
            % (plot.x(i), plot.H - plot.mb + 18, esc(stamp[5:]))  # MM-DD
            for i, (stamp, _) in enumerate(points)
            if i % step == 0 or i == plot.n - 1]


def last_value_callout(plot, points):
    if not plot.n:
        return ""
    lx, lv = plot.x(plot.n - 1), points[-1][1]
    ly = plot.y(lv)
    return ('<circle cx="%.1f" cy="%.1f" r="4.5" fill="#f6c344"/>'
            '<text x="%.1f" y="%.1f" font-size="13" font-weight="700" '
            'fill="#d4960b" text-anchor="end">%s</text>'
            % (lx, ly, lx, ly - 12, fmt_full(lv)))


def build_svg(points, total_last_week):
    plot = Plot(points)
    line_pts, area_pts = series_points(plot, points)

    svg = """<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-label="Weekly downloads of the DuckDB mssql community extension">
  <defs>
    <linearGradient id="area" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#f6c344" stop-opacity="0.35"/>
      <stop offset="100%" stop-color="#f6c344" stop-opacity="0.02"/>
    </linearGradient>
  </defs>
  <rect x="0" y="0" width="{W}" height="{H}" rx="10" fill="#ffffff" stroke="#e3e8ee"/>
  <text x="{ml}" y="26" font-size="15" font-weight="700" fill="#2b3banner">{title}</text>
  <text x="{rightx}" y="26" font-size="12" fill="#8a94a6" text-anchor="end">{total}/wk total</text>
  {grid}
  {labels_y}
  {area}
  <polyline points="{line}" fill="none" stroke="#f6c344" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>
  {dots}
  {callout}
  {labels_x}
  <text x="{ml}" y="{H_minus}" font-size="10" fill="#aab2c0">Source: duckdb.org/community_extensions/download_metrics &#183; downloads in the trailing 7 days, sampled weekly</text>
</svg>
""".format(
        W=plot.W,
        H=plot.H,
        ml=plot.ml,
        rightx=plot.W - plot.mr,
        title="DuckDB mssql extension &#8212; weekly downloads",
        total=fmt_full(total_last_week),
        grid="\n  ".join(grid_lines(plot)),
        labels_y="\n  ".join(y_axis_labels(plot)),
        area=('<polygon points="%s" fill="url(#area)"/>' % area_pts) if area_pts else "",
        line=line_pts,
        dots="\n  ".join(dot_markers(plot, points)),
        callout=last_value_callout(plot, points),
        labels_x="\n  ".join(x_axis_labels(plot, points)),
        H_minus=plot.H - 10,
    ).replace("#2b3banner", "#2b3a4a")
    return svg


def nice_ceil(v):
    if v <= 0:
        return 1
    import math

    exp = math.floor(math.log10(v))
    base = 10 ** exp
    for mult in (1, 2, 2.5, 5, 10):
        if v <= mult * base:
            return int(mult * base) if mult * base >= 1 else mult * base
    return int(10 * base)


def fmt_short(v):
    v = float(v)
    if v >= 1000:
        return ("%.1fk" % (v / 1000)).replace(".0k", "k")
    return "%d" % v


def fmt_full(v):
    return "{:,}".format(int(v))


# --------------------------------------------------------------------------- #
# README block
# --------------------------------------------------------------------------- #
def build_badges_block():
    """Shields badge row, h3-duckdb style, placed above the H1."""
    repo = REPO
    ci = (
        "[![CI](https://github.com/%s/actions/workflows/ci.yml/badge.svg)]"
        "(https://github.com/%s/actions/workflows/ci.yml)" % (repo, repo)
    )
    duckdb = (
        "[![DuckDB](https://img.shields.io/static/v1?label=duckdb&message=%s%%2B&color=blue)]"
        "(https://github.com/duckdb/duckdb/releases)" % DUCKDB_MIN_VERSION
    )
    release = (
        "[![Latest release](https://img.shields.io/github/v/release/%s?label=release&color=blue)]"
        "(https://github.com/%s/releases/latest)" % (repo, repo)
    )
    downloads = (
        "[![Community downloads per week]"
        "(https://img.shields.io/badge/dynamic/json"
        "?url=https%3A%2F%2Fcommunity-extensions.duckdb.org%2Fdownloads-last-week.json"
        "&query=%24." + EXTENSION + "&label=downloads%2Fweek&color=brightgreen)]"
        "(https://duckdb.org/community_extensions/download_metrics)"
    )
    license_badge = (
        "[![License: %s](https://img.shields.io/badge/License-%s-blue.svg)](LICENSE)"
        % (LICENSE_NAME, LICENSE_NAME)
    )
    stars = (
        "[![GitHub stars](https://img.shields.io/github/stars/%s?style=flat&color=informational)]"
        "(https://github.com/%s/stargazers)" % (repo, repo)
    )
    return "\n".join(
        [BADGES_BEGIN, "", ci, duckdb, release, downloads, license_badge, stars, "", BADGES_END]
    )


def pages_url():
    """GitHub Pages URL for the repo (owner/repo -> owner.github.io/repo/)."""
    owner, _, repo = REPO.partition("/")
    return "https://%s.github.io/%s/" % (owner, repo)


def build_chart_block(version, total_last_week):
    """The chart section. Deliberately carries NO numbers of its own.

    Anything written here can only change by committing to `main`, which is
    protected — so the freshness lives outside the file instead: the chart is an
    image served from GitHub Pages and regenerated weekly, and the downloads
    figure is a dynamic shields badge in the row above. `version` and
    `total_last_week` stay in the signature because the caller has them and a
    future block may want them; today they are only logged.
    """
    return "\n".join(
        [
            CHART_BEGIN,
            "",
            "### 📈 Community Extension Downloads",
            "",
            "[![Weekly downloads of the mssql DuckDB community extension](%sdownload-metrics.svg)](%s)"
            % (pages_url(), "https://duckdb.org/community_extensions/download_metrics"),
            "",
            "📊 **[Interactive chart](%s)** — queried live in your browser with DuckDB-Wasm." % pages_url(),
            "",
            "> Regenerated weekly and served from GitHub Pages, so it stays current without a commit. "
            "Counts are a Cloudflare estimate of `INSTALL mssql FROM community` events, aggregated "
            "across DuckDB versions and platforms. "
            "Source: [DuckDB Community Extensions download metrics](https://duckdb.org/community_extensions/download_metrics).",
            "",
            CHART_END,
        ]
    )


def replace_region(text, begin, end, block):
    pattern = re.compile(re.escape(begin) + r".*?" + re.escape(end), re.DOTALL)
    return pattern.sub(lambda _m: block, text, count=1)


def splice_badges(text, block):
    """Replace the badge region, or prepend it above everything (the H1)."""
    if BADGES_BEGIN in text and BADGES_END in text:
        return replace_region(text, BADGES_BEGIN, BADGES_END, block)
    return block + "\n\n" + text.lstrip("\n")


def splice_chart(text, block):
    """Replace the chart region, or insert it after the lead paragraph."""
    if CHART_BEGIN in text and CHART_END in text:
        return replace_region(text, CHART_BEGIN, CHART_END, block)
    # lead paragraph = H1 title + description (first two blank-line groups).
    parts = text.split("\n\n", 2)
    if len(parts) >= 2:
        head = parts[0] + "\n\n" + parts[1]
        rest = "\n\n" + parts[2] if len(parts) == 3 else ""
        return head + "\n\n" + block + rest
    return text.rstrip() + "\n\n" + block + "\n"


# --------------------------------------------------------------------------- #
# --------------------------------------------------------------------------- #
# self-test
# --------------------------------------------------------------------------- #
# The chart is generated on every Pages deploy and nothing looks at it, so a
# renderer that emitted malformed XML or drew outside its own box would publish
# a broken image silently. These checks need no network and no DuckDB.

#: (label, point count, value for sample i)
SERIES_CASES = [
    ("no samples at all", 0, lambda i: 0),
    ("a single sample", 1, lambda i: 1234),
    ("two samples, one zero", 2, lambda i: [0, 7][i]),
    ("every sample zero", 10, lambda i: 0),
    ("a flat series", 8, lambda i: 500),
    ("a year of weekly samples", 52, lambda i: i * 137),
    ("one spike among small values", 20, lambda i: 10 ** 6 if i == 7 else 3),
    ("values in the millions", 12, lambda i: 10 ** 7 + i),
    ("fractional values", 6, lambda i: i * 1.5),
]

#: (input, nice_ceil's answer) -- the axis top, so a wrong one mislabels
#: every gridline.
# `(2.5, 2)` pins a LATENT FLAW rather than the right answer: at mult=2.5 and
# base=1 the `int(mult * base)` truncation returns 2 for an input of 2.5, an
# axis top BELOW the maximum value, which would put y(2.5) above the plot. It
# is unreachable from this script's data -- download counts are integers, and
# no integer falls in (2, 2.5] -- so it is recorded, not changed: this PR is a
# refactor and must not alter behaviour. Worth deciding separately.
CEIL_CASES = [(0, 1), (-5, 1), (1, 1), (2, 2), (2.5, 2), (3, 5), (5, 5),
              (6, 10), (10, 10), (11, 20), (999, 1000), (1001, 2000),
              (24000, 25000), (250001, 500000)]

#: (input, fmt_short's answer) -- the y-axis tick text.
SHORT_CASES = [(0, "0"), (999, "999"), (1000, "1k"), (1500, "1.5k"),
               (10000, "10k"), (12345, "12.3k")]


def _series(n, gen):
    d0 = datetime.date(2026, 1, 4)
    return [((d0 + datetime.timedelta(weeks=i)).isoformat(), gen(i)) for i in range(n)]


def _line(ok, label, detail):
    print("  [%s] %-50s -> %s" % ("ok" if ok else "FAIL", label, detail))
    return 0 if ok else 1


#: Which axis each coordinate attribute belongs to. The first cut of the
#: bounds check compared EVERY value against max(W, H) = 760, so a y of 300-760
#: -- geometry drawn well below a 280-tall canvas -- passed; and it read only
#: cx/cy/x1/y1/x2/y2, so the <text> x/y (the callout's `ly - 12` among them) and
#: the polyline/polygon `points` were never looked at (roborev on 35e6ed3).
X_ATTRS = ("cx", "x1", "x2", "x")
Y_ATTRS = ("cy", "y1", "y2", "y")
#: The lookbehind matters: without it the bare `x` alternative matches the `x`
#: inside `viewBox="0 0 760 280"` and `rx="10"`, which is how the first attempt
#: at this check reported every chart as unparseable.
NUMBER = re.compile(r'(?<![A-Za-z-])(' + "|".join(X_ATTRS + Y_ATTRS) + r')="([^"]*)"')
POINTS = re.compile(r'points="([^"]*)"')


def svg_coordinates(svg):
    """(x values, y values, values that are not finite numbers).

    `points` pairs are split and attributed by position, so the series and the
    area polygon are checked like every other coordinate. A value that does not
    parse as a finite float is RETURNED rather than skipped: `nan` and `inf`
    are exactly what a division gone wrong emits, and dropping them is how such
    a chart would pass.
    """
    xs, ys, bad = [], [], []

    def take(axis, raw):
        try:
            value = float(raw)
        except ValueError:
            bad.append(raw)
            return
        if value != value or value in (float("inf"), float("-inf")):
            bad.append(raw)
            return
        axis.append(value)

    for match in NUMBER.finditer(svg):
        take(xs if match.group(1) in X_ATTRS else ys, match.group(2))
    for match in POINTS.finditer(svg):
        for pair in match.group(1).split():
            parts = pair.split(",")
            if len(parts) != 2:
                bad.append(pair)
                continue
            take(xs, parts[0])
            take(ys, parts[1])
    return xs, ys, bad


def _check_renders():
    """Every series shape must produce well-formed XML inside its own box.

    Parsing is the real oracle here: a chart nobody opens is exactly where a
    stray quote or an unclosed tag survives. The bounds check catches the
    other silent failure -- geometry that divides wrong and draws off-canvas.
    """
    import xml.etree.ElementTree as ET
    print("the chart renders, for every shape of series:")
    failures = 0
    for label, n, gen in SERIES_CASES:
        points = _series(n, gen)
        svg = build_svg(points, 4321)
        inside = False
        try:
            ET.fromstring(svg)
        except ET.ParseError as exc:
            detail = "XML ERROR: %s" % exc
        else:
            xs, ys, bad = svg_coordinates(svg)
            plot = Plot(points)
            over = ([v for v in xs if not -1 <= v <= plot.W + 1]
                    + [v for v in ys if not -1 <= v <= plot.H + 1])
            inside = not over and not bad
            detail = "%d bytes, %d x / %d y" % (len(svg), len(xs), len(ys))
            if bad:
                detail += ", UNPARSEABLE: %s" % bad[:3]
            if over:
                detail += ", OUT OF BOUNDS: %s" % over[:3]
        failures += _line(inside, label, detail)
    return failures


def _check_coordinate_reader():
    """svg_coordinates() itself, since the render checks lean on it.

    Each case is something the first cut of the bounds check got wrong: it
    compared y against max(W, H), never read <text> or `points`, and its
    `[-\d.]+` pattern silently DROPPED nan/inf rather than failing on them
    (roborev on 35e6ed3).
    """
    print("\nsvg_coordinates():")
    failures = 0
    xs, ys, bad = svg_coordinates(
        '<svg viewBox="0 0 760 280" width="760">'
        '<rect rx="10" x="5" y="7"/>'
        '<line x1="1" y1="2" x2="3" y2="4"/>'
        '<circle cx="11" cy="13"/>'
        '<text x="17" y="19">x</text>'
        '<polyline points="21,23 29,31"/></svg>')
    failures += _line(sorted(xs) == [1.0, 3.0, 5.0, 11.0, 17.0, 21.0, 29.0],
                      "every x attribute and the points' x halves", str(sorted(xs)))
    failures += _line(sorted(ys) == [2.0, 4.0, 7.0, 13.0, 19.0, 23.0, 31.0],
                      "every y attribute and the points' y halves", str(sorted(ys)))
    failures += _line(bad == [],
                      "viewBox and rx are NOT read as x coordinates", "none")

    _, _, bad = svg_coordinates('<circle cx="nan" cy="inf"/><text x="-inf" y="q"/>')
    failures += _line(sorted(bad) == ["-inf", "inf", "nan", "q"],
                      "nan / inf / junk are reported, never dropped", str(sorted(bad)))

    _, _, bad = svg_coordinates('<polyline points="1,2 3"/>')
    failures += _line(bad == ["3"], "a half point pair is reported", str(bad))
    return failures


def _check_geometry():
    print("\nthe geometry:")
    failures = 0
    one = Plot(_series(1, lambda i: 5))
    failures += _line(one.x(0) == one.ml + one.pw / 2,
                      "a single sample sits in the middle, not at the edge",
                      "%.1f" % one.x(0))
    many = Plot(_series(5, lambda i: 5))
    failures += _line(many.x(0) == many.ml and abs(many.x(4) - (many.ml + many.pw)) < 1e-9,
                      "the first and last samples sit on the plot edges",
                      "%.1f..%.1f" % (many.x(0), many.x(4)))
    failures += _line(many.y(0) == many.baseline and many.y(many.vmax_nice) == many.mt,
                      "zero is the baseline and the axis top is the top",
                      "%.1f..%.1f" % (many.y(0), many.y(many.vmax_nice)))
    zeros = Plot(_series(4, lambda i: 0))
    failures += _line(zeros.vmax_nice >= 1 and zeros.y(0) == zeros.baseline,
                      "an all-zero series still has a non-zero axis",
                      "vmax=%s" % zeros.vmax_nice)
    return failures


def _check_labels():
    print("\nthe labels and the callout:")
    failures = 0
    points = _series(52, lambda i: i * 137)
    plot = Plot(points)
    labels = x_axis_labels(plot, points)
    # step = max(1, n // 8) gives 9 evenly spaced ticks for n = 52, and the
    # last sample is labelled whether or not it lands on one -- so 10.
    failures += _line(len(labels) == 10, "52 samples are thinned to 10 x labels",
                      "%d label(s)" % len(labels))
    failures += _line(points[-1][0][5:] in labels[-1],
                      "the last sample is always labelled", points[-1][0][5:])
    callout = last_value_callout(plot, points)
    failures += _line(fmt_full(points[-1][1]) in callout,
                      "the callout names the last value", fmt_full(points[-1][1]))
    failures += _line(last_value_callout(Plot([]), []) == "",
                      "an empty series has no callout", "''")
    failures += _line(len(grid_lines(plot)) == 5 and len(y_axis_labels(plot)) == 5,
                      "five gridlines, five y labels", "5 / 5")
    return failures


def _check_scales():
    print("\nnice_ceil (the axis top) and fmt_short (the tick text):")
    failures = 0
    for value, want in CEIL_CASES:
        got = nice_ceil(value)
        failures += _line(got == want, "nice_ceil(%s)" % value, "%s (want %s)" % (got, want))
    for value, want in SHORT_CASES:
        got = fmt_short(value)
        failures += _line(got == want, "fmt_short(%s)" % value, "%r (want %r)" % (got, want))
    failures += _line(fmt_full(1234567) == "1,234,567", "fmt_full groups thousands",
                      fmt_full(1234567))
    return failures


def self_test():
    failures = _check_renders()
    failures += _check_coordinate_reader()
    failures += _check_geometry()
    failures += _check_labels()
    failures += _check_scales()
    print("\n%s: self-test, %d failure(s)" % ("FAIL" if failures else "PASS", failures))
    return failures


USAGE = "usage: update_download_metrics.py [--svg-only PATH | --self-test]"


def parse_args(args):
    """-> (svg path, update the README, exit status).

    The status is None when the run should proceed.
    """
    if args and args[0] == "--svg-only":
        if len(args) != 2:
            print(USAGE, file=sys.stderr)
            return None, False, 2
        return args[1], False, None
    if args:
        print(USAGE, file=sys.stderr)
        return None, False, 2
    return SVG, True, None


def write_svg(path, points, total_last_week):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(build_svg(points, total_last_week))
    print("wrote %s" % path)


def update_readme(version, total_last_week):
    with open(README, encoding="utf-8") as fh:
        text = fh.read()
    # chart first (lead-paragraph logic runs before badges shift the top),
    # then badges (prepended above the H1).
    new_text = splice_chart(text, build_chart_block(version, total_last_week))
    new_text = splice_badges(new_text, build_badges_block())
    if new_text == text:
        print("README.md unchanged")
        return
    with open(README, "w", encoding="utf-8") as fh:
        fh.write(new_text)
    print("README.md updated")


def main():
    args = sys.argv[1:]
    if args and args[0] == "--self-test":
        return 1 if self_test() else 0
    svg_out, readme, status = parse_args(args)
    if status is not None:
        return status

    try:
        total_last_week = fetch_total_last_week()
        points = fetch_series()
    except (DuckDBError, RuntimeError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    version = latest_release()
    print("version=%s total_last_week=%s weekly_points=%d"
          % (version, total_last_week, len(points)))

    write_svg(svg_out, points, total_last_week)
    if readme:
        update_readme(version, total_last_week)
    return 0


if __name__ == "__main__":
    sys.exit(main())
