#!/usr/bin/env python3
"""Update README badges, version and the download-metrics chart.

The data is fetched and aggregated *with DuckDB itself* (dogfooding) — the same
`read_json` queries the DuckDB Community Extensions metrics page documents:

  - https://community-extensions.duckdb.org/downloads-last-week.json
        rolling "downloads in the last 7 days" snapshot, keyed by extension name.
  - https://community-extensions.duckdb.org/download-stats-weekly/{year}/{week}.json
        the same snapshot archived once per ISO week -> our time series.

DuckDB (httpfs + read_json) does the fetch/aggregation; this script renders the
result into docs/assets/download-metrics.svg (a self-contained SVG line chart
that GitHub renders inline — README HTML is sanitized, so no JS/WASM there) and
rewrites the managed block in README.md delimited by
  <!-- DOWNLOAD-METRICS:START --> ... <!-- DOWNLOAD-METRICS:END -->

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


def build_svg(points, total_last_week):
    W, H = 760, 280
    ml, mr, mt, mb = 56, 24, 44, 48
    pw, ph = W - ml - mr, H - mt - mb

    values = [v for _, v in points] or [0]
    vmax = max(values) or 1
    vmax_nice = nice_ceil(vmax)
    n = len(points)

    def x(i):
        if n <= 1:
            return ml + pw / 2
        return ml + pw * i / (n - 1)

    def y(v):
        return mt + ph * (1 - v / vmax_nice)

    grid = []
    labels_y = []
    for g in range(5):
        gv = vmax_nice * g / 4
        gy = y(gv)
        grid.append(
            '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" '
            'stroke="#e3e8ee" stroke-width="1"/>' % (ml, gy, ml + pw, gy)
        )
        labels_y.append(
            '<text x="%.1f" y="%.1f" font-size="11" fill="#8a94a6" '
            'text-anchor="end" dominant-baseline="middle">%s</text>'
            % (ml - 8, gy, fmt_short(gv))
        )

    line_pts = " ".join("%.1f,%.1f" % (x(i), y(v)) for i, (_, v) in enumerate(points))
    area_pts = (
        "%.1f,%.1f " % (x(0), mt + ph)
        + line_pts
        + " %.1f,%.1f" % (x(n - 1), mt + ph)
    ) if n else ""

    dots = []
    for i, (_, v) in enumerate(points):
        dots.append(
            '<circle cx="%.1f" cy="%.1f" r="2.6" fill="#fff" '
            'stroke="#f6c344" stroke-width="2"/>' % (x(i), y(v))
        )

    # x labels: thinned to ~8 ticks
    labels_x = []
    step = max(1, n // 8)
    for i, (stamp, _) in enumerate(points):
        if i % step == 0 or i == n - 1:
            labels_x.append(
                '<text x="%.1f" y="%.1f" font-size="10" fill="#8a94a6" '
                'text-anchor="middle">%s</text>'
                % (x(i), H - mb + 18, esc(stamp[5:]))  # MM-DD
            )

    # last value callout
    callout = ""
    if n:
        lx, lv = x(n - 1), points[-1][1]
        ly = y(lv)
        callout = (
            '<circle cx="%.1f" cy="%.1f" r="4.5" fill="#f6c344"/>'
            '<text x="%.1f" y="%.1f" font-size="13" font-weight="700" '
            'fill="#d4960b" text-anchor="end">%s</text>'
            % (lx, ly, lx, ly - 12, fmt_full(lv))
        )

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
        W=W,
        H=H,
        ml=ml,
        rightx=W - mr,
        title="DuckDB mssql extension &#8212; weekly downloads",
        total=fmt_full(total_last_week),
        grid="\n  ".join(grid),
        labels_y="\n  ".join(labels_y),
        area=('<polygon points="%s" fill="url(#area)"/>' % area_pts) if area_pts else "",
        line=line_pts,
        dots="\n  ".join(dots),
        callout=callout,
        labels_x="\n  ".join(labels_x),
        H_minus=H - 10,
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


def build_chart_block(version, total_last_week):
    today = datetime.date.today().isoformat()
    return "\n".join(
        [
            CHART_BEGIN,
            "",
            "### 📈 Community Extension Downloads",
            "",
            "![Weekly downloads of the mssql DuckDB community extension](docs/assets/download-metrics.svg)",
            "",
            "> Latest published version **v%s** · **%s** downloads in the trailing 7 days "
            "(snapshot %s UTC). Counts are a Cloudflare estimate of `INSTALL mssql FROM community` "
            "events, aggregated across DuckDB versions and platforms. "
            "Source: [DuckDB Community Extensions download metrics](https://duckdb.org/community_extensions/download_metrics)."
            % (version, fmt_full(total_last_week), today),
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
def main():
    try:
        total_last_week = fetch_total_last_week()
        points = fetch_series()
    except (DuckDBError, RuntimeError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    version = latest_release()
    print("version=%s total_last_week=%s weekly_points=%d"
          % (version, total_last_week, len(points)))

    # SVG
    os.makedirs(os.path.dirname(SVG), exist_ok=True)
    svg = build_svg(points, total_last_week)
    with open(SVG, "w", encoding="utf-8") as fh:
        fh.write(svg)

    # README
    with open(README, encoding="utf-8") as fh:
        text = fh.read()
    # chart first (lead-paragraph logic runs before badges shift the top),
    # then badges (prepended above the H1).
    new_text = splice_chart(text, build_chart_block(version, total_last_week))
    new_text = splice_badges(new_text, build_badges_block())
    if new_text != text:
        with open(README, "w", encoding="utf-8") as fh:
            fh.write(new_text)
        print("README.md updated")
    else:
        print("README.md unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
