#!/usr/bin/env python3
"""Generate/check the portable Marauder source/workflow ledger.

Bundled inventory and editorial status are sufficient by default. Optional
--upstream PATH verifies a clean pinned checkout. This is not proof of RF parity.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INVENTORY = ROOT / "docs/MARAUDER_SOURCE_INVENTORY.json"
STATUS = ROOT / "docs/MARAUDER_COVERAGE_STATUS.json"
OUTPUT = ROOT / "docs/MARAUDER_COVERAGE_LEDGER.json"
MARKDOWN = ROOT / "docs/MARAUDER_COVERAGE_LEDGER.md"
PIN = "511bb3694d0c4dc064888e762bde44fcc9b7f48a"
DEVELOP_PIN = "79cc6bcba1db6679baee1dab2b0c1ad5010e6363"
REPOSITORY = "https://github.com/justcallmekoko/ESP32Marauder"
STATUSES = {"partial", "pending", "needs_review", "hardware_limited"}
WORKSTREAMS = {"profiles_and_lists", "storage_and_transfer", "settings_and_device_control",
              "gps_geofence_and_poi", "dynamic_menu_options", "integrated_wardriving"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def local_file(relative: str) -> None:
    path = Path(relative)
    require(not path.is_absolute() and ".." not in path.parts, f"nonportable path: {relative}")
    resolved = (ROOT / path).resolve()
    require(resolved.is_relative_to(ROOT.resolve()) and resolved.is_file(),
            f"missing local evidence: {relative}")


def verify_source(inventory: dict, upstream: Path | None = None) -> None:
    require(inventory["format"] == 1 and inventory["revision"] == PIN, "unexpected inventory revision/schema")
    require(inventory["upstream"] == REPOSITORY, "unexpected source repository")
    require([len(inventory[k]) for k in ("modes", "literal_menu_entries", "cli_commands")] == [95, 265, 58],
            "source inventory counts changed; review classification")
    require(set(inventory["source_sha256"]) == {
        "WiFiScan.h", "MenuFunctions.cpp", "CommandLine.cpp", "CommandLine.h", "lang_var.h"
    }, "unexpected source file set")
    for digest in inventory["source_sha256"].values():
        require(isinstance(digest, str) and len(digest) == 64 and
                all(c in "0123456789abcdef" for c in digest), "invalid source digest")
    symbols = [row["symbol"] for row in inventory["modes"]]
    require(len(set(symbols)) == 95, "duplicate source mode")
    values: dict[str, list[str]] = {}
    for row in inventory["modes"]:
        require(type(row["source_line"]) is int and row["source_line"] > 0 and
                type(row["upstream_value"]) is int, "invalid source line/value")
        values.setdefault(str(row["upstream_value"]), []).append(row["symbol"])
    require({k: v for k, v in values.items() if len(v) > 1} == inventory["upstream_numeric_collisions"],
            "numeric collision inventory mismatch")
    if upstream is None:
        return
    def git(*args: str) -> str:
        return subprocess.check_output(["git", "-C", str(upstream.resolve()), *args],
                                       text=True, encoding="utf-8").strip()
    require(git("rev-parse", "HEAD") in (PIN, DEVELOP_PIN), "checkout is not an audited revision")
    require(not git("status", "--porcelain", "--untracked-files=no"), "upstream has tracked changes")
    for name, expected in inventory["source_sha256"].items():
        source = upstream / "esp32_marauder" / name
        require(source.is_file() and hashlib.sha256(source.read_bytes()).hexdigest() == expected,
                f"upstream source digest mismatch: {name}")


def indexes_valid(row: dict, inventory: dict) -> None:
    for key, source in (("related_menu_entries", "literal_menu_entries"),
                        ("related_cli_commands", "cli_commands")):
        indexes = row[key]
        require(isinstance(indexes, list) and len(indexes) == len(set(indexes)) and
                all(type(i) is int and 0 <= i < len(inventory[source]) for i in indexes),
                f"invalid {key}: {row.get('symbol', row.get('name'))}")


def generate(inventory: dict, status: dict) -> dict:
    require(status["format"] == 1 and set(status["status_definitions"]) == STATUSES, "unexpected status schema")
    rows = status["modes"]
    require(len(rows) == 95 and {m["symbol"] for m in rows} == {m["symbol"] for m in inventory["modes"]},
            "every source mode must have exactly one status record")
    by_symbol = {m["symbol"]: m for m in rows}
    modes = []
    for source in inventory["modes"]:
        row = dict(by_symbol[source["symbol"]])
        require(row["status"] in STATUSES, "unknown mode status")
        for key in ("family", "status_detail", "next_gap", "s3_role", "c5_role"):
            require(isinstance(row[key], str) and bool(row[key].strip()), f"missing {key}")
        indexes_valid(row, inventory)
        local_file(row["cerberus_evidence"])
        for path in row.get("cerberus_code", []):
            local_file(path)
        row.update(upstream_value=source["upstream_value"], upstream_source={
            "path": "esp32_marauder/WiFiScan.h", "line": source["source_line"],
            "url": f"{REPOSITORY}/blob/{PIN}/esp32_marauder/WiFiScan.h#L{source['source_line']}",
        })
        modes.append(row)
    workstreams = status["non_mode_workstreams"]
    require(len(workstreams) == 6 and {w["name"] for w in workstreams} == WORKSTREAMS, "unexpected workstreams")
    for row in workstreams:
        require(row["status"] in STATUSES and bool(row["next_gap"].strip()), "invalid workstream status/gap")
        indexes_valid(row, inventory)
        if row["name"] == "integrated_wardriving":
            require(row.get("external_reference") == "docs/WARDRIVE_RECENT_SOURCE_CONTRACT.md" and
                    not row["related_menu_entries"] and not row["related_cli_commands"],
                    "standalone Wardriving must not invent Marauder menu provenance")
            local_file(row["external_reference"])
        else:
            require(row["related_menu_entries"] and row["related_cli_commands"], "missing workstream references")
    return {
        "format": 2, "audit_checkpoint": status["checkpoint"], "scope": status["scope"],
        "contribution_summary": "docs/CONTRIBUTION_REV93.md",
        "prior_public_contribution": "docs/CONTRIBUTION_REV81.md",
        "upstream": REPOSITORY, "upstream_revision": PIN, "equivalent_develop_revision": DEVELOP_PIN,
        "upstream_source_sha256": inventory["source_sha256"], "status_definitions": status["status_definitions"],
        "mode_count": len(modes), "status_counts": dict(sorted(Counter(m["status"] for m in modes).items())),
        "status_count_unit": "source mode symbols, not distinct features or completion percentage",
        "modes": modes, "non_mode_workstreams": workstreams,
        "literal_menu_entries": inventory["literal_menu_entries"], "cli_commands": inventory["cli_commands"],
        "upstream_numeric_collisions": inventory["upstream_numeric_collisions"],
    }


def markdown(ledger: dict) -> str:
    def cell(text: str) -> str:
        return text.replace("|", "\\|").replace("\n", " ")
    lines = ["# Marauder coverage ledger", "",
        f"Current editorial checkpoint: **{ledger['audit_checkpoint']}**. "
        "This is a source/workflow audit, not a claim that all modes work or a completion percentage. "
        "[Dual-radio development](DUAL_RADIO_SCANNING.md) separates current source/host checks "
        "from installation and physical acceptance. "
        "[Rev93 contribution](CONTRIBUTION_REV93.md) retains its original contribution scope. "
        "[Earlier contribution evidence](CONTRIBUTION_REV81.md) keeps its original scope.", "",
        "The inventory contains **95 mode symbols, 265 literal menu entries and 58 top-level CLI entries**. "
        "These overlap. Some symbols are control states or dormant legacy modes; labels alone do not prove reachable services.", "",
        "| Status | Symbols | Meaning |", "| --- | ---: | --- |"]
    for name, count in ledger["status_counts"].items():
        lines.append(f"| {name} | {count} | {cell(ledger['status_definitions'][name])} |")
    lines += ["", "## Source and reproduction", "",
        f"The [inventory](MARAUDER_SOURCE_INVENTORY.json) is pinned to [{PIN[:8]}]({REPOSITORY}/tree/{PIN}); "
        f"reviewed develop [{DEVELOP_PIN[:8]}]({REPOSITORY}/tree/{DEVELOP_PIN}) has the same source tree. "
        "[Baseline notes](MARAUDER_DEVELOP_BASELINE.md) explain that relationship. Numeric value 85 is shared "
        "by BT_SCAN_IBEACON and BT_FINDMY_SOUND, so symbols remain audit keys.", "",
        "Edit [MARAUDER_COVERAGE_STATUS.json](MARAUDER_COVERAGE_STATUS.json), then run "
        "`python tools/build_marauder_coverage_ledger.py`. `--check` rejects stale output. "
        "No adjacent private workspace is needed. Optional `--upstream /path/to/ESP32Marauder` "
        "also verifies the clean pinned checkout and recorded source hashes.", "",
        "## Modes", "",
        "The [JSON ledger](MARAUDER_COVERAGE_LEDGER.json) also contains code references, S3/C5 roles "
        "and zero-based menu/CLI indexes. Separate workstreams below are not included in mode counts.", "",
        "| Mode | Status | Evidence | Remaining scope |", "| --- | --- | --- | --- |"]
    for row in ledger["modes"]:
        evidence = row["cerberus_evidence"].removeprefix("docs/")
        lines.append(f"| `{row['symbol']}` | {row['status']} | [notes]({evidence}) | {cell(row['next_gap'])} |")
    lines += ["", "## Work outside mode constants", "",
              "| Workstream | Status | Remaining scope |", "| --- | --- | --- |"]
    for row in ledger["non_mode_workstreams"]:
        lines.append(f"| {row['name']} | {row['status']} | {cell(row['next_gap'])} |")
    lines += ["", "Integrated Wardriving references separate Cerberus firmware through its "
        "[source contract](WARDRIVE_RECENT_SOURCE_CONTRACT.md), not an extra Marauder mode. "
        "Receive-side validation does not establish active-feature acceptance. Earlier active Wi-Fi errors, "
        "disabled S3 native extended reception, C5 extended-receive hardware acceptance, "
        "physical GPS/expansion validation and full parity remain open.", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check generated files without writing")
    parser.add_argument("--upstream", type=Path, help="optionally verify a clean pinned Marauder checkout")
    args = parser.parse_args()
    try:
        inventory = json.loads(INVENTORY.read_text(encoding="utf-8"))
        status = json.loads(STATUS.read_text(encoding="utf-8"))
        verify_source(inventory, args.upstream)
        ledger = generate(inventory, status)
        outputs = {OUTPUT: json.dumps(ledger, ensure_ascii=False, indent=2) + "\n", MARKDOWN: markdown(ledger)}
        for path, content in outputs.items():
            if args.check:
                require(path.is_file() and path.read_text(encoding="utf-8") == content,
                        f"stale generated file: {path.relative_to(ROOT)}")
            else:
                path.write_text(content, encoding="utf-8", newline="\n")
        print(f"Coverage ledger {'checked' if args.check else 'generated'}: {ledger['mode_count']} modes, "
              f"{len(ledger['non_mode_workstreams'])} workstreams; {ledger['status_counts']}")
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as exc:
        print(f"Coverage ledger error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
