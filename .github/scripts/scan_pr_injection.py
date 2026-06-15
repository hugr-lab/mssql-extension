#!/usr/bin/env python3
"""Scan a GitHub PR's prose surfaces for prompt-injection attempts.

The threat model is an AI reviewer (e.g. /ultrareview, GitHub Copilot
Workspace, a future `@claude review` bot) that reads PR content and follows
instructions hidden in it. Hostile content can live in:

    * The PR title and body
    * Commit messages
    * Issue comments and review comments
    * The diff itself (strings, doc additions)
    * New / modified Markdown files

We fetch all of those via the GitHub REST API -- never by checking out the
PR's code, since this workflow runs on `pull_request_target` with write
permissions. The PR's code never executes; we only treat its text as inert
strings to scan.

Detection uses ProtectAI's `llm-guard` PromptInjection scanner, which wraps
a fine-tuned DeBERTa-v3 model. Findings are emitted as GitHub workflow
warnings (with file/line annotations where possible) and as a job-summary
table. The job's exit code is non-zero if any finding's risk score exceeds
the configured threshold, so the check shows red on the PR.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import textwrap
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

# llm-guard is heavy (pulls torch + transformers). Import lazily so failures
# show a clear message rather than an import-error backtrace.
try:
    from llm_guard.input_scanners import PromptInjection
    from llm_guard.input_scanners.prompt_injection import MatchType
except ImportError as e:
    print(f"::error::llm-guard not installed: {e}", file=sys.stderr)
    sys.exit(2)


# Scoring: PromptInjection returns a float in [0, 1]. >0.9 is almost always
# a real injection; 0.5-0.9 includes false positives on adversarial-sounding
# prose (e.g. discussion of injection itself). We warn at >=0.5 and fail at
# >=0.9 so the same scanner can report informationally on this very PR
# without blocking it.
WARN_THRESHOLD = float(os.environ.get("INJECTION_WARN_THRESHOLD", "0.5"))
FAIL_THRESHOLD = float(os.environ.get("INJECTION_FAIL_THRESHOLD", "0.9"))

# Long inputs are chunked by llm-guard internally with MatchType.SENTENCE,
# but we still cap any single source at 200 KB so a 50 MB binary blob in a
# diff doesn't OOM the runner.
MAX_BYTES_PER_SOURCE = 200_000


@dataclass
class Finding:
    source: str       # "PR body", "comment #123", "src/foo.cpp diff", ...
    snippet: str      # first ~300 chars of the matching chunk
    score: float
    url: str = ""     # optional clickable link back to the source

    def severity(self) -> str:
        if self.score >= FAIL_THRESHOLD:
            return "error"
        return "warning"


def gh(args: list[str]) -> str:
    """Run `gh api` and return stdout. Errors abort the scan.

    Captures bytes and decodes with errors="replace": a PR diff can legitimately
    contain non-UTF-8 bytes (binary files, or UTF-16 surrogate halves in test
    fixtures) and a strict decode would crash the scan instead of scanning the
    text. Replacement characters don't hide injection text in the readable parts.
    """
    res = subprocess.run(["gh", *args], capture_output=True)
    if res.returncode != 0:
        stderr = res.stderr.decode("utf-8", errors="replace")
        print(f"::error::gh {' '.join(args)} failed: {stderr}", file=sys.stderr)
        sys.exit(2)
    return res.stdout.decode("utf-8", errors="replace")


def collect_sources(repo: str, pr_number: int) -> list[tuple[str, str, str]]:
    """Return [(source_label, text, url), ...] for everything to scan."""
    sources: list[tuple[str, str, str]] = []

    # PR title + body
    pr = json.loads(gh(["api", f"repos/{repo}/pulls/{pr_number}"]))
    title = pr.get("title") or ""
    body = pr.get("body") or ""
    sources.append(("PR title", title, pr["html_url"]))
    sources.append(("PR body", body, pr["html_url"]))

    # Issue comments (top-level PR conversation)
    comments = json.loads(gh(["api", f"repos/{repo}/issues/{pr_number}/comments"]))
    for c in comments:
        label = f"comment #{c['id']} by @{c['user']['login']}"
        sources.append((label, c.get("body") or "", c["html_url"]))

    # Review comments (inline, attached to diff)
    review_comments = json.loads(
        gh(["api", f"repos/{repo}/pulls/{pr_number}/comments"])
    )
    for c in review_comments:
        label = f"review comment #{c['id']} by @{c['user']['login']} on {c.get('path', '?')}"
        sources.append((label, c.get("body") or "", c["html_url"]))

    # Commit messages
    commits = json.loads(gh(["api", f"repos/{repo}/pulls/{pr_number}/commits"]))
    for c in commits:
        sha = c["sha"][:8]
        msg = c["commit"]["message"]
        sources.append((f"commit {sha}", msg, c["html_url"]))

    # The diff itself. The "diff" media type gives unified diff text -- this
    # is the right surface for "injection hidden in a string literal" or
    # "instructions added to a Markdown doc".
    diff = gh(
        [
            "api",
            "-H",
            "Accept: application/vnd.github.v3.diff",
            f"repos/{repo}/pulls/{pr_number}",
        ]
    )
    sources.append(("PR diff", diff, pr["html_url"] + "/files"))

    return sources


def truncate(text: str, limit: int = MAX_BYTES_PER_SOURCE) -> str:
    if len(text.encode("utf-8")) <= limit:
        return text
    truncated = text.encode("utf-8")[:limit].decode("utf-8", errors="ignore")
    return truncated + "\n\n[... truncated for scanner; full source on PR page ...]"


def scan(sources: list[tuple[str, str, str]]) -> list[Finding]:
    # MatchType.SENTENCE splits on sentence boundaries; each sentence is
    # scored individually so a 50 KB doc with one hostile sentence still
    # surfaces the hostile sentence cleanly. Use the lowest threshold the
    # scanner allows so we get scores for everything; we apply our own
    # WARN/FAIL thresholds below.
    scanner = PromptInjection(threshold=0.001, match_type=MatchType.SENTENCE)
    findings: list[Finding] = []

    for label, text, url in sources:
        if not text.strip():
            continue
        text = truncate(text)
        _sanitized, is_valid, score = scanner.scan(text)
        if is_valid:
            continue
        if score < WARN_THRESHOLD:
            continue
        snippet = textwrap.shorten(text.strip().replace("\n", " "), width=280)
        findings.append(Finding(source=label, snippet=snippet, score=score, url=url))

    return findings


def emit_annotations(findings: Iterable[Finding]) -> None:
    for f in findings:
        # GitHub workflow command. The annotation surfaces in the Files
        # Changed tab when path/line are set; for prose surfaces (PR body,
        # comments) it lands in the Checks tab.
        sev = f.severity()
        msg = f"prompt-injection score {f.score:.2f} in {f.source}: {f.snippet}"
        print(f"::{sev}::{msg}")


def emit_job_summary(findings: list[Finding]) -> None:
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return
    with open(summary_path, "a", encoding="utf-8") as fh:
        if not findings:
            fh.write("## Prompt-injection scan\n\nNo findings.\n")
            return
        fh.write("## Prompt-injection scan\n\n")
        fh.write(
            f"Threshold: warn at {WARN_THRESHOLD}, fail at {FAIL_THRESHOLD}.\n\n"
        )
        fh.write("| Severity | Score | Source | Snippet |\n")
        fh.write("|---|---|---|---|\n")
        for f in findings:
            snippet = f.snippet.replace("|", "\\|")
            link = f"[{f.source}]({f.url})" if f.url else f.source
            fh.write(f"| {f.severity()} | {f.score:.2f} | {link} | {snippet} |\n")
        fh.write("\n")
        fh.write(
            "Detector: ProtectAI llm-guard PromptInjection (DeBERTa-v3). "
            "False positives on prose that *discusses* injection patterns "
            "(e.g. this very security tooling PR) are expected; treat the "
            "summary as advisory, not a verdict.\n"
        )


def main() -> int:
    repo = os.environ["GITHUB_REPOSITORY"]
    pr_number_env = os.environ.get("PR_NUMBER") or os.environ.get("GITHUB_REF_NAME")
    if not pr_number_env:
        print("::error::PR_NUMBER not set", file=sys.stderr)
        return 2
    pr_number = int(pr_number_env)

    sources = collect_sources(repo, pr_number)
    print(f"Scanning {len(sources)} sources from PR #{pr_number}...")
    findings = scan(sources)
    findings.sort(key=lambda f: f.score, reverse=True)

    emit_annotations(findings)
    emit_job_summary(findings)

    if not findings:
        print("No prompt-injection signals detected.")
        return 0

    print(f"Found {len(findings)} flagged source(s).")
    worst = findings[0].score
    if worst >= FAIL_THRESHOLD:
        print(f"::error::Highest risk score {worst:.2f} >= fail threshold {FAIL_THRESHOLD}.")
        return 1
    print(f"Highest score {worst:.2f} below fail threshold {FAIL_THRESHOLD}; advisory only.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
