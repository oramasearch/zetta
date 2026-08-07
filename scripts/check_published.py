#!/usr/bin/env python3
"""Report local checkpoints and corpora that are NOT published to the private HF org.

The publish rule already lived in CLAUDE.md and the hf-ops skill, and was still missed for three
checkpoints in a row. A rule you have to remember is not a control; this makes it answerable in
one command.

Reads HF_TOKEN from the environment. Never prints it. Read-only — it uploads nothing.

    python3 scripts/check_published.py            # report
    python3 scripts/check_published.py --quiet    # exit 1 if anything is unpublished
"""
import json
import os
import re
import sys
import urllib.request
from pathlib import Path

MODEL_REPO = "OramaSearch/base-agent-0.6b"
DATASET_REPO = "OramaSearch/base-agent-interaction-corpus"


def remote_files(repo: str, kind: str, token: str) -> set[str]:
    url = f"https://huggingface.co/api/{kind}/{repo}"
    request = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(request, timeout=30) as response:
        payload = json.load(response)
    if not payload.get("private", False):
        # A public product repo is a leak, not a warning. Say so loudly.
        print(f"!! {repo} is PUBLIC — it must be private", file=sys.stderr)
    return {entry["rfilename"] for entry in payload.get("siblings", [])}


def main() -> int:
    token = os.environ.get("HF_TOKEN")
    if not token:
        print("HF_TOKEN not set; cannot check", file=sys.stderr)
        return 2

    root = Path(__file__).resolve().parent.parent
    missing: list[str] = []

    published_versions = {
        match for name in remote_files(MODEL_REPO, "models", token)
        for match in re.findall(r"^(v\d+)/", name)
    }
    for directory in sorted((root / "data" / "checkpoints").glob("*interaction-v*")):
        version = directory.name.rsplit("-", 1)[-1]
        if version not in published_versions:
            missing.append(f"checkpoint {directory.name} -> {MODEL_REPO}/{version}")

    dataset_files = remote_files(DATASET_REPO, "datasets", token)
    for corpus in sorted((root / "data" / "corpora").glob("interaction-corpus-v*.jsonl")):
        version = re.search(r"(v\d+)\.jsonl$", corpus.name)
        if version and not any(version.group(1) in name for name in dataset_files):
            missing.append(f"corpus {corpus.name} -> {DATASET_REPO}")

    if missing:
        print("UNPUBLISHED — a checkpoint may not name a corpus that was never published:")
        for entry in missing:
            print(f"  {entry}")
        return 1

    print(f"all local checkpoints and corpora are published ({len(published_versions)} versions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
