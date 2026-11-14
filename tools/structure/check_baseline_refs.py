#!/usr/bin/env python3
"""Baseline reference integrity for the structural guard.

`structure-baseline.json` is a reviewed *exact* ledger: every exception names a
path that must still exist. Nothing regenerates it automatically (see the
`policy` field), so when a file is deleted the entry silently becomes dead
weight — the guard only notices when its full pipeline runs, and that pipeline
needs a Windows PowerShell + clang-cl inventory producer.

This script checks the part that needs no build artifacts at all: every
exception whose `path` looks like a repository file must still resolve. Names
carrying a `target:` / `test:` prefix are CMake targets and CTest names; they
cannot be verified offline and are reported as such instead of being guessed.

Exit codes: 0 = clean, 1 = stale entries found (only with --strict), 2 = the
baseline itself is unusable.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

FILE = "file"
TARGET = "target"
TEST = "test"


def classify(value: object) -> str:
    text = str(value or "")
    if text.startswith("target:"):
        return TARGET
    if text.startswith("test:"):
        return TEST
    return FILE


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when any file-path exception is stale")
    parser.add_argument("--output", type=Path, default=None,
                        help="optional JSON report path")
    args = parser.parse_args()

    try:
        baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
        exceptions = baseline["exceptions"]
        protected = baseline.get("protectedModules", {})
    except (OSError, KeyError, json.JSONDecodeError) as error:
        print(f"baseline unusable: {error}", file=sys.stderr)
        return 2

    kinds = Counter(classify(entry.get("path")) for entry in exceptions)
    stale: list[dict] = []
    per_path: Counter[str] = Counter()
    per_rule: Counter[str] = Counter()
    for entry in exceptions:
        if classify(entry.get("path")) != FILE:
            continue
        path = str(entry.get("path"))
        if (args.root / path).exists():
            continue
        stale.append(entry)
        per_path[path] += 1
        per_rule[str(entry.get("rule"))] += 1

    protected_missing = [
        (module, path)
        for module, paths in protected.items()
        for path in paths
        if not (args.root / path).exists()
    ]

    # 分类计数必须与总数对上——审计里吃过的亏是「分组之和 != 总数」时
    # 另一套记账已经混进来了。
    accounted = kinds[FILE] + kinds[TARGET] + kinds[TEST]
    if accounted != len(exceptions):
        print(f"baseline unclassified entries: {len(exceptions) - accounted}",
              file=sys.stderr)
        return 2

    print(f"exceptions        {len(exceptions):5d}  "
          f"(file {kinds[FILE]} / target {kinds[TARGET]} / test {kinds[TEST]})")
    print(f"protectedModules  {len(protected):5d}  missing {len(protected_missing)}")
    print(f"stale file paths  {len(stale):5d}  distinct {len(per_path)}")
    for path, count in per_path.most_common():
        print(f"  {count:3d} entries  {path}")
    if per_rule:
        print("by rule: " + ", ".join(f"{rule}={count}"
                                      for rule, count in per_rule.most_common()))
    print(f"offline-unverifiable: target {kinds[TARGET]} + test {kinds[TEST]} "
          f"= {kinds[TARGET] + kinds[TEST]} names (need the CMake/CTest inventory)")

    if args.output:
        report = {
            "schema": "ConcordStructureBaselineRefs/v1",
            "exceptions": len(exceptions),
            "kinds": dict(kinds),
            "protectedModules": len(protected),
            "protectedMissing": [{"module": m, "path": p} for m, p in protected_missing],
            "staleEntries": len(stale),
            "stalePaths": [{"path": p, "entries": c} for p, c in per_path.most_common()],
            "staleByRule": dict(per_rule),
            "offlineUnverifiable": kinds[TARGET] + kinds[TEST],
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")

    failing = bool(stale) or bool(protected_missing)
    if failing and args.strict:
        print("BREACH: baseline references a path that no longer exists", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
