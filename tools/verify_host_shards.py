#!/usr/bin/env python3
"""Verify complete Windows host coverage from four isolated CI artifacts.

This validates receipts and their actual suite logs. It does not run fixtures,
compile firmware, or equate modeled host checks with hardware acceptance.
CI retries require "Re-run all jobs": successful shards from older attempts
are deliberately not combined. Use --local only for clean local shard receipts.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys

import run_host_checks as runner

SHARD_COUNT = 4


class VerificationError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_digest(value: object) -> str:
    return digest(json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8"))


def valid_digest(value: object) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def verify(receipt_paths: list[Path], expected_head: str,
           expected_inputs: dict[str, str], expected_execution: dict) -> dict:
    require(re.fullmatch(r"[0-9a-f]{40}", expected_head) is not None, "Expected an exact Git commit")
    local = expected_execution == {"kind": "local"}
    github = (set(expected_execution) == {"kind", "run_id", "run_attempt"} and
        expected_execution.get("kind") == "github_actions" and
        all(isinstance(expected_execution[k], str) and
            re.fullmatch(r"[1-9][0-9]*", expected_execution[k]) is not None
            for k in ("run_id", "run_attempt")))
    require(local or github, "Expected an explicit local or exact GitHub run/attempt context")
    require(len(receipt_paths) == SHARD_COUNT,
            'Expected exactly four Windows shard receipts for this execution; CI retries require "Re-run all jobs"')
    names = runner.registered_checks()
    registry = runner.registry_sha256()
    covered: set[str] = set()
    indices: set[int] = set()
    common_inputs = None
    common_python = None
    evidence = []
    total_duration = 0.0
    for path in receipt_paths:
        raw = path.read_bytes()
        receipt = json.loads(raw)
        require(isinstance(receipt, dict), f"{path}: receipt is not an object")
        require(type(receipt.get("format")) is int and receipt["format"] == 1,
                f"{path}: unsupported receipt format")
        require(receipt.get("status") == "passed" and receipt.get("suite_set") == "rev144",
                f"{path}: shard did not pass the current suite set")
        require(receipt.get("hardware_validated") is False, f"{path}: unexpected hardware claim")
        require(receipt.get("platform") == "win32", f"{path}: not a Windows shard")
        require(receipt.get("source_head") == receipt.get("source_head_after") == expected_head,
                f"{path}: mixed, changed, or unexpected source HEAD")
        require(receipt.get("source_clean") is True and receipt.get("source_clean_after") is True,
                f"{path}: shard checkout was not clean")
        require(receipt.get("execution") == expected_execution,
                f"{path}: mixed or unexpected execution context/run/attempt")
        require(receipt.get("inputs_changed") == [], f"{path}: inputs changed during execution")
        require(receipt.get("registry_sha256") == registry and
                type(receipt.get("registered_suite_count")) is int and
                receipt["registered_suite_count"] == len(names), f"{path}: wrong registry")
        require(receipt.get("ziglang_version") == "0.14.1", f"{path}: wrong compiler pin")
        python = receipt.get("python_version")
        require(isinstance(python, str) and re.fullmatch(r"3\.\d+\.\d+", python) is not None,
                f"{path}: missing Python version")
        if common_python is None:
            common_python = python
        require(python == common_python, f"{path}: mixed Python versions")
        selection = receipt.get("selection")
        require(isinstance(selection, dict), f"{path}: missing shard selection")
        index = selection.get("shard_index")
        require(type(index) is int and 0 <= index < SHARD_COUNT, f"{path}: invalid shard index")
        require(index not in indices, f"{path}: duplicate shard index")
        require(selection.get("mode") == "shard" and
                type(selection.get("shard_count")) is int and selection["shard_count"] == SHARD_COUNT,
                f"{path}: not a four-way shard")
        selected = list(runner.selected_checks(index, SHARD_COUNT))
        require(selection.get("selected_names") == selected and
                type(selection.get("selected_suite_count")) is int and
                selection["selected_suite_count"] == len(selected), f"{path}: wrong selected suites")
        inputs = receipt.get("inputs_after")
        require(isinstance(inputs, dict) and bool(inputs), f"{path}: missing input map")
        require(inputs == expected_inputs, f"{path}: input bytes differ from verifier checkout")
        for name, value in inputs.items():
            relative = PurePosixPath(name)
            require(relative.as_posix() == name and not relative.is_absolute() and
                    ".." not in relative.parts and "\\" not in name and ":" not in name and
                    valid_digest(value), f"{path}: invalid input binding")
        if common_inputs is None:
            common_inputs = inputs
        require(inputs == common_inputs, f"{path}: mixed source/configuration bytes")
        checks = receipt.get("checks")
        require(isinstance(checks, list) and all(isinstance(c, dict) for c in checks),
                f"{path}: invalid check results")
        require([c.get("check") for c in checks] == selected, f"{path}: missing, repeated, or reordered result")
        duration = 0.0
        for check in checks:
            name = check["check"]
            require(name not in covered, f"{path}: overlapping suite {name}")
            require(type(check.get("exit_code")) is int and check["exit_code"] == 0,
                    f"{path}: failed suite {name}")
            elapsed = check.get("duration_seconds")
            require(type(elapsed) in (int, float) and math.isfinite(elapsed) and elapsed >= 0,
                    f"{path}: invalid duration for {name}")
            log_name = name.removesuffix(".py") + ".log"
            require(check.get("log") == "artifacts/host-checks/" + log_name and
                    valid_digest(check.get("log_sha256")), f"{path}: invalid log binding for {name}")
            log = path.parent / log_name
            require(not log.is_symlink() and log.is_file(), f"{path}: missing local log for {name}")
            require(digest(log.read_bytes()) == check["log_sha256"], f"{path}: changed log for {name}")
            covered.add(name)
            duration += elapsed
        indices.add(index)
        total_duration += duration
        evidence.append({"receipt": str(path), "sha256": digest(raw), "shard_index": index,
                         "suite_count": len(selected), "suite_seconds_sum": round(duration, 6)})
    require(indices == set(range(SHARD_COUNT)), "Missing Windows shard")
    require(covered == set(names), "Windows shards do not cover all 181 registered suites")
    return {"format": 1, "status": "passed", "suite_set": "rev144", "platform": "win32",
            "hardware_validated": False, "source_head": expected_head, "registry_sha256": registry,
            "execution": expected_execution,
            "shard_count": SHARD_COUNT, "verified_suite_count": len(covered),
            "verified_input_count": len(common_inputs), "inputs_sha256": canonical_digest(common_inputs),
            "suite_seconds_sum": round(total_duration, 6),
            "receipts": sorted(evidence, key=lambda item: item["shard_index"])}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifacts", type=Path, help="Downloaded, separately extracted Windows artifacts")
    parser.add_argument("--expected-head", required=True, help="Exact checked-out workflow commit")
    context = parser.add_mutually_exclusive_group(required=True)
    context.add_argument("--expected-run-id", help="Exact GitHub run; requires --expected-run-attempt")
    context.add_argument("--local", action="store_true", help="Verify clean local shards without GitHub IDs")
    parser.add_argument("--expected-run-attempt", help='Current GitHub attempt; retries require "Re-run all jobs"')
    parser.add_argument("--output", type=Path,
                        default=runner.ROOT / "artifacts/host-checks/windows-aggregate.json")
    args = parser.parse_args()
    if (args.local and args.expected_run_attempt is not None or
            not args.local and args.expected_run_attempt is None):
        parser.error("Use --local alone or both --expected-run-id and --expected-run-attempt")
    execution = ({"kind": "local"} if args.local else {"kind": "github_actions",
        "run_id": args.expected_run_id, "run_attempt": args.expected_run_attempt})
    try:
        require(runner.source_head() == args.expected_head, "Verifier checkout differs from expected HEAD")
        require(runner.source_clean(), "Verifier requires a clean Git checkout")
        from build_unified import inputs
        # .gitattributes fixes text to LF and preserves binary bytes, so all
        # platforms must match this exact complete source/configuration map.
        result = verify(sorted(args.artifacts.rglob("host-checks.json")), args.expected_head, inputs(), execution)
    except (OSError, ValueError, TypeError, KeyError) as error:
        result = {"format": 1, "status": "failed", "hardware_validated": False, "error": str(error)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
