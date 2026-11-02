#!/usr/bin/env python3
"""Turn the guest serial protocol into reproducible POSIX conformance metrics."""

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path

BEGIN = re.compile(r"POSIX_BEGIN seed=(\d+) cases=(\d+)")
CASE = re.compile(r"POSIX_CASE seed=(\d+) index=(\d+) api=([a-z_]+) status=(pass|fail)(?: (.*))?")
END = re.compile(r"POSIX_END executed=(\d+) failed=(\d+)")


def parse_log(log: str, qemu_status: int) -> dict:
    begin = BEGIN.search(log)
    end = END.search(log)
    cases = []
    seen = set()
    for match in CASE.finditer(log):
        seed, index, api, status, detail = match.groups()
        key = (int(seed), int(index))
        if key in seen:
            continue
        seen.add(key)
        cases.append({"seed": int(seed), "index": int(index), "api": api,
                      "status": status, "detail": detail or ""})
    seed = int(begin.group(1)) if begin else None
    requested = int(begin.group(2)) if begin else None
    summary = {"executed": int(end.group(1)), "failed": int(end.group(2))} if end else None
    counts = defaultdict(lambda: {"pass": 0, "fail": 0})
    deviations = defaultdict(lambda: {"count": 0, "example": None})
    for case in cases:
        counts[case["api"]][case["status"]] += 1
        if case["status"] == "fail":
            key = (case["api"], case["detail"])
            deviations[key]["count"] += 1
            deviations[key]["example"] = deviations[key]["example"] or case
    issues = []
    if begin is None:
        issues.append("missing POSIX_BEGIN")
    if end is None:
        issues.append("missing POSIX_END: guest crash, hang or truncated run")
    if qemu_status == 124:
        issues.append("QEMU timeout")
    if qemu_status not in (0, 124):
        issues.append(f"QEMU exit status {qemu_status}")
    if summary and summary["executed"] != len(cases):
        issues.append("case count differs from guest summary")
    if summary and summary["failed"] != sum(c["status"] == "fail" for c in cases):
        issues.append("failure count differs from guest summary")
    if requested is not None and summary and summary["executed"] != requested:
        issues.append("incomplete run")
    return {"schema": 1, "seed": seed, "requested": requested,
            "observed": len(cases), "passed": sum(c["status"] == "pass" for c in cases),
            "failed": sum(c["status"] == "fail" for c in cases),
            "qemu_status": qemu_status, "complete": not issues,
            "infrastructure_issues": issues, "by_api": dict(sorted(counts.items())),
            "unique_deviations": [
                {"api": api, "detail": detail, **value}
                for (api, detail), value in sorted(deviations.items())],
            "failures": [c for c in cases if c["status"] == "fail"]}


def markdown(report: dict) -> str:
    lines = ["# POSIX syscall fuzz report", "",
             f"Seed: `{report['seed']}`. Observed: {report['observed']}/{report['requested']}. "
             f"Passed: {report['passed']}. Failed: {report['failed']}. "
             f"Unique deviations: {len(report['unique_deviations'])}.", "",
             "| API scenario | Pass | Fail |", "|---|---:|---:|"]
    for api, counts in report["by_api"].items():
        lines.append(f"| `{api}` | {counts['pass']} | {counts['fail']} |")
    if report["infrastructure_issues"]:
        lines += ["", "## Run issues", ""]
        lines += [f"- {issue}" for issue in report["infrastructure_issues"]]
    if report["unique_deviations"]:
        lines += ["", "## Deviations", ""]
        for deviation in report["unique_deviations"]:
            case = deviation["example"]
            lines.append(f"- `{case['api']}`: {deviation['detail']} "
                         f"({deviation['count']} cases; example {case['index']}; "
                         f"replay: `make fuzz-posix FUZZ_SEED={case['seed']} "
                         f"FUZZ_ONLY={case['index']}`)")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    parser.add_argument("--qemu-status", type=int, default=0)
    args = parser.parse_args()
    report = parse_log(args.log.read_text(errors="replace"), args.qemu_status)
    args.json.write_text(json.dumps(report, indent=2) + "\n")
    args.markdown.write_text(markdown(report))
    print(f"POSIX fuzz: {report['passed']} pass, {report['failed']} fail; "
          f"report: {args.markdown}")
    for issue in report["infrastructure_issues"]:
        print(f"run issue: {issue}")
    return 0 if report["complete"] and not report["failures"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
