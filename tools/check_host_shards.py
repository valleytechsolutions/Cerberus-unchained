#!/usr/bin/env python3
"""Exercise host selection, receipt publication and aggregate rejection paths.

Only the fixture subprocess boundary is modeled in runner tests. The production
runner, deterministic registry selection, file/log hashes and verifier execute.
No compiler, radio, firmware build or external service is used.
"""
from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import re
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import build_unified
import run_host_checks as runner
import verify_host_shards as verifier

HEAD = "a" * 40
EXECUTION = {"kind": "github_actions", "run_id": "12345678901", "run_attempt": "2"}


class SelectionChecks(unittest.TestCase):
    def test_execution_context_is_local_or_exact_github_pair(self):
        with patch.dict(runner.os.environ, {}, clear=True):
            self.assertEqual(runner.execution_context(), {"kind": "local"})
        with patch.dict(runner.os.environ, {"GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "2"}, clear=True):
            self.assertEqual(runner.execution_context(),
                             {"kind": "github_actions", "run_id": "123", "run_attempt": "2"})
        for environment in ({"GITHUB_RUN_ID": "123"}, {"GITHUB_RUN_ATTEMPT": "2"},
                            {"GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "0"}):
            with self.subTest(environment=environment), patch.dict(runner.os.environ, environment, clear=True), \
                 self.assertRaises(ValueError):
                runner.execution_context()

    def test_four_shards_partition_exact_canonical_registry(self):
        names = runner.registered_checks()
        self.assertEqual(len(names), 197)
        self.assertEqual(names.count("check_radio_worker.py"), 1)
        self.assertIn("check_radio_worker.py", runner.CHECKS)
        for name in ("check_mesh_mac.py", "check_mesh_observation.py", "check_mesh_link.py", "check_mesh_scan.py",
                     "check_mesh_bridge.py", "check_mesh_report.py", "check_mesh_archive.py", "check_mesh_ui.py",
                     "check_mesh_locate.py", "check_mesh_locate_link.py", "check_mesh_locate_report.py",
                     "check_mesh_locate_bridge.py"):
            self.assertEqual(names.count(name), 1)
            self.assertIn(name, runner.CHECKS)
        parts = [runner.selected_checks(i, 4) for i in range(4)]
        self.assertEqual([len(part) for part in parts], [50, 49, 49, 49])
        self.assertEqual(set().union(*(set(part) for part in parts)), set(names))
        for index, part in enumerate(parts):
            self.assertEqual(part, tuple(name for ordinal, name in enumerate(names) if ordinal % 4 == index))
            self.assertEqual(len(set(part) & set(runner.CHECKS)), [43, 43, 42, 42][index])
            self.assertEqual(len(set(part) & set(runner.SERIAL_CHECKS)), [7, 6, 7, 7][index])
            for other in parts[index + 1:]:
                self.assertFalse(set(part) & set(other))
        self.assertEqual(runner.selected_checks(), names)

    def test_invalid_or_partial_shard_arguments_reject(self):
        for index, count in ((None, 4), (0, None), (-1, 4), (4, 4), (0, 0),
                             (0, 198), (True, 4), (0, True)):
            with self.subTest(index=index, count=count), self.assertRaises(ValueError):
                runner.selected_checks(index, count)

    def test_registry_digest_binds_order_and_serial_boundary(self):
        original = runner.registry_sha256()
        parallel, serial = runner.CHECKS, runner.SERIAL_CHECKS
        with patch.object(runner, "CHECKS", (runner.CHECKS[1], runner.CHECKS[0], *runner.CHECKS[2:])):
            self.assertNotEqual(runner.registry_sha256(), original)
        with patch.object(runner, "CHECKS", parallel[:-1]), \
             patch.object(runner, "SERIAL_CHECKS", (parallel[-1], *serial)):
            self.assertEqual(runner.selected_checks(), (*runner.CHECKS, *runner.SERIAL_CHECKS))
            self.assertNotEqual(runner.registry_sha256(), original)
        with patch.object(runner, "CHECKS", (runner.CHECKS[0], *runner.CHECKS[:-1])):
            with self.assertRaises(ValueError):
                runner.registered_checks()

    def test_workflow_has_four_isolated_windows_shards_and_full_ubuntu(self):
        workflow = (runner.ROOT / ".github/workflows/host-checks.yml").read_text()
        args = re.findall(r"shard_args: '--shard-index (\d+) --shard-count (\d+)'", workflow)
        self.assertEqual(args, [(str(i), "4") for i in range(4)])
        self.assertIn("label: ubuntu-full\n            shard_args: ''", workflow)
        self.assertIn("needs: host-checks\n    if: always()", workflow)
        self.assertIn("merge-multiple: false", workflow)
        self.assertIn('python tools/verify_host_shards.py artifacts/windows-host-shards', workflow)
        self.assertIn('--expected-head "${{ github.sha }}"', workflow)
        self.assertIn('--expected-run-id "${{ github.run_id }}"', workflow)
        self.assertIn('--expected-run-attempt "${{ github.run_attempt }}"', workflow)
        self.assertEqual(workflow.count("${{ github.run_number }}-attempt-${{ github.run_attempt }}"), 3)
        self.assertIn('Retry with "Re-run all jobs"', workflow)
        self.assertIn("MATRIX_RESULT: ${{ needs.host-checks.result }}", workflow)
        for name in ("Check build-report failure handling", "Check SD resources"):
            self.assertIn(name + "\n        if: matrix.os == 'ubuntu-latest' || matrix.label == 'windows-0'", workflow)


class RunnerChecks(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "tools").mkdir()
        for name in runner.registered_checks():
            (self.root / "tools" / name).write_text("# modeled child-process boundary\n")
        self.output = self.root / "artifacts/host-checks"
        self.bound = {"tools/run_host_checks.py": "b" * 64}

    def invoke(self, arguments=(), *, fail=None, after=None, heads=(HEAD, HEAD),
               clean=(True, True), interrupt=False):
        commands = []
        def child(command, **kwargs):
            name = Path(command[-1]).name
            commands.append(name)
            if interrupt:
                raise KeyboardInterrupt
            return SimpleNamespace(returncode=1 if name == fail else 0,
                                   stdout=(name + " boundary completed\n").encode())
        with patch.object(runner, "ROOT", self.root), patch.object(runner, "OUTPUT", self.output), \
             patch.object(runner, "execution_context", return_value=EXECUTION), \
             patch.object(runner, "source_head", side_effect=heads), \
             patch.object(runner, "source_clean", side_effect=clean), \
             patch.object(runner.subprocess, "run", side_effect=child), \
             patch.object(runner.importlib.util, "find_spec", return_value=object()), \
             patch.object(runner.importlib.metadata, "version", return_value="0.14.1"), \
             patch.object(build_unified, "inputs", side_effect=[self.bound, self.bound if after is None else after]), \
             patch("sys.argv", ["run_host_checks.py", "--jobs", "2", *arguments]), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            status = runner.main()
        return status, json.loads((self.output / "host-checks.json").read_text()), commands

    def test_default_fullrun_keeps_format_and_all_checks(self):
        status, receipt, commands = self.invoke()
        self.assertEqual(status, 0)
        self.assertEqual(receipt["format"], 1)
        self.assertEqual(receipt["suite_set"], "rev144")
        self.assertEqual(receipt["status"], "passed")
        self.assertEqual(receipt["selection"]["mode"], "full")
        self.assertEqual([c["check"] for c in receipt["checks"]], list(runner.registered_checks()))
        self.assertEqual(set(commands), set(runner.registered_checks()))
        self.assertEqual(commands[len(runner.CHECKS):], list(runner.SERIAL_CHECKS))
        self.assertEqual(receipt["inputs_after"], self.bound)
        self.assertFalse(receipt["hardware_validated"])
        self.assertEqual(receipt["execution"], EXECUTION)
        for result in receipt["checks"]:
            self.assertGreaterEqual(result["duration_seconds"], 0)
            self.assertEqual(verifier.digest((self.root / result["log"]).read_bytes()), result["log_sha256"])

    def test_each_shard_runs_only_selected_checks_with_serial_order(self):
        for index in range(4):
            with self.subTest(index=index):
                status, receipt, commands = self.invoke(("--shard-index", str(index), "--shard-count", "4"))
                selected = runner.selected_checks(index, 4)
                self.assertEqual(status, 0)
                self.assertEqual(receipt["selection"]["shard_index"], index)
                self.assertEqual(receipt["selection"]["selected_names"], list(selected))
                self.assertEqual([c["check"] for c in receipt["checks"]], list(selected))
                self.assertEqual(set(commands), set(selected))
                serial = [n for n in selected if n in runner.SERIAL_CHECKS]
                self.assertEqual(commands[-len(serial):], serial)

    def test_suite_failure_or_changed_inputs_or_head_cannot_pass(self):
        cases = ({"fail": runner.CHECKS[0]}, {"after": {"tools/run_host_checks.py": "c" * 64}},
                 {"heads": (HEAD, "b" * 40)})
        for case in cases:
            with self.subTest(case=case):
                status, receipt, _ = self.invoke(**case)
                self.assertEqual(status, 1)
                self.assertEqual(receipt["status"], "failed")

    def test_interrupted_run_removes_prior_success_receipt(self):
        self.output.mkdir(parents=True)
        path = self.output / "host-checks.json"
        path.write_text('{"status":"passed"}')
        with self.assertRaises(KeyboardInterrupt):
            self.invoke(interrupt=True)
        self.assertFalse(path.exists())

    def test_fullrun_without_git_is_preserved_but_shard_requires_head(self):
        status, receipt, _ = self.invoke(heads=(None, None))
        self.assertEqual(status, 0)
        self.assertIsNone(receipt["source_head"])
        with self.assertRaises(SystemExit) as raised:
            self.invoke(("--shard-index", "0", "--shard-count", "4"), heads=(None,))
        self.assertEqual(raised.exception.code, 2)

    def test_shards_reject_dirty_start_and_fail_if_checkout_becomes_dirty(self):
        args = ("--shard-index", "0", "--shard-count", "4")
        with self.assertRaises(SystemExit) as raised:
            self.invoke(args, clean=(False,))
        self.assertEqual(raised.exception.code, 2)
        status, receipt, _ = self.invoke(args, clean=(True, False))
        self.assertEqual(status, 1)
        self.assertEqual(receipt["status"], "failed")
        self.assertFalse(receipt["source_clean_after"])


class AggregateChecks(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.inputs = {"tools/run_host_checks.py": "c" * 64,
                       ".github/workflows/host-checks.yml": "d" * 64}
        self.paths = []
        for index in range(4):
            folder = self.root / f"windows-{index}"
            folder.mkdir()
            names = list(runner.selected_checks(index, 4))
            checks = []
            for name in names:
                content = (name + " completed\n").encode()
                log_name = name.removesuffix(".py") + ".log"
                (folder / log_name).write_bytes(content)
                checks.append({"check": name, "exit_code": 0, "duration_seconds": 1.25,
                    "log": "artifacts/host-checks/" + log_name, "log_sha256": verifier.digest(content)})
            receipt = {"format": 1, "status": "passed", "suite_set": "rev144", "platform": "win32",
                "hardware_validated": False, "source_head": HEAD, "source_head_after": HEAD,
                "source_clean": True, "source_clean_after": True,
                "execution": EXECUTION,
                "inputs_changed": [], "inputs_after": self.inputs, "python_version": "3.13.16",
                "ziglang_version": "0.14.1", "registry_sha256": runner.registry_sha256(),
                "registered_suite_count": 197, "checks": checks,
                "selection": {"mode": "shard", "shard_index": index, "shard_count": 4,
                    "selected_names": names, "selected_suite_count": len(names)}}
            path = folder / "host-checks.json"
            path.write_text(json.dumps(receipt))
            self.paths.append(path)

    def verify(self, paths=None, execution=EXECUTION):
        return verifier.verify(self.paths if paths is None else paths, HEAD, self.inputs, execution)

    def rejected_change(self, mutate, index=0):
        path = self.paths[index]
        original = path.read_bytes()
        receipt = json.loads(original)
        mutate(receipt)
        path.write_text(json.dumps(receipt))
        try:
            with self.assertRaises(verifier.VerificationError):
                self.verify()
        finally:
            path.write_bytes(original)

    def test_complete_logs_and_shards_pass_in_any_artifact_order(self):
        report = self.verify(list(reversed(self.paths)))
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["verified_suite_count"], 197)
        self.assertEqual(report["suite_seconds_sum"], 197 * 1.25)
        self.assertEqual([r["shard_index"] for r in report["receipts"]], [0, 1, 2, 3])
        self.assertEqual(report["inputs_sha256"], verifier.canonical_digest(self.inputs))
        for row, path in zip(report["receipts"], self.paths):
            self.assertEqual(row["sha256"], verifier.digest(path.read_bytes()))

    def test_missing_extra_duplicate_or_unknown_shard_rejected(self):
        for paths in ([], self.paths[:3], [*self.paths, self.paths[0]], [self.paths[0]] * 4):
            with self.subTest(paths=len(paths)), self.assertRaises(verifier.VerificationError):
                self.verify(paths)
        for field, value in (("shard_index", 4), ("shard_index", True), ("shard_count", 1),
                             ("mode", "full"), ("selected_suite_count", 34)):
            self.rejected_change(lambda r, f=field, v=value: r["selection"].update({f: v}))

    def test_bad_provenance_status_registry_or_toolchain_rejected(self):
        changes = {"format": True, "status": "failed", "suite_set": "rev129", "platform": "linux",
                   "hardware_validated": True, "source_head": "b" * 40, "source_head_after": "b" * 40,
                   "source_clean": False, "source_clean_after": False,
                   "inputs_changed": ["file.c"], "registry_sha256": "0" * 64,
                   "registered_suite_count": 139, "ziglang_version": "0.14.0", "python_version": "3.13.17"}
        for key, value in changes.items():
            with self.subTest(field=key):
                self.rejected_change(lambda r, k=key, v=value: r.update({k: v}))

    def test_missing_changed_or_invalid_input_bindings_rejected(self):
        for value in ({}, {"tools/run_host_checks.py": "c" * 64},
                      {**self.inputs, "extra.c": "e" * 64},
                      {**self.inputs, "tools/run_host_checks.py": "e" * 64},
                      {**self.inputs, "tools/run_host_checks.py": "bad"}):
            with self.subTest(inputs=value):
                self.rejected_change(lambda r, v=value: r.update(inputs_after=v), index=1)

    def test_all_four_identically_modified_inputs_reject_verifier_checkout(self):
        for path in self.paths:
            receipt = json.loads(path.read_text())
            receipt["inputs_after"]["tools/run_host_checks.py"] = "f" * 64
            path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(verifier.VerificationError, "verifier checkout"):
            self.verify()

    def test_mixed_runs_attempts_and_implicit_context_rejected(self):
        for value in ({**EXECUTION, "run_id": "999"}, {**EXECUTION, "run_attempt": "1"},
                      {"kind": "local"}, {}, None):
            with self.subTest(execution=value):
                self.rejected_change(lambda r, v=value: r.update(execution=v))
        for value in ({}, {**EXECUTION, "run_id": ""}, {**EXECUTION, "run_attempt": "0"}):
            with self.subTest(expected=value), self.assertRaises(verifier.VerificationError):
                self.verify(execution=value)

    def test_explicit_local_shards_do_not_fabricate_github_ids(self):
        for path in self.paths:
            receipt = json.loads(path.read_text())
            receipt["execution"] = {"kind": "local"}
            path.write_text(json.dumps(receipt))
        self.assertEqual(self.verify(execution={"kind": "local"})["execution"], {"kind": "local"})
        with self.assertRaises(verifier.VerificationError):
            self.verify()

    def test_incomplete_overlapping_or_reordered_suite_results_rejected(self):
        self.rejected_change(lambda r: r["checks"].pop())
        self.rejected_change(lambda r: r["checks"].append(copy.deepcopy(r["checks"][0])))
        self.rejected_change(lambda r: r["checks"].reverse())
        self.rejected_change(lambda r: r["checks"][0].update(check=runner.selected_checks(1, 4)[0]))
        self.rejected_change(lambda r: r["selection"]["selected_names"].reverse())
        self.rejected_change(lambda r: r["selection"]["selected_names"].pop())

    def test_failure_bad_log_or_duration_rejected(self):
        for key, value in (("exit_code", 1), ("exit_code", False), ("log_sha256", "0" * 64),
                           ("log", "../outside.log"), ("duration_seconds", -1),
                           ("duration_seconds", float("nan")), ("duration_seconds", True)):
            with self.subTest(field=key, value=value):
                self.rejected_change(lambda r, k=key, v=value: r["checks"][0].update({k: v}))
        receipt = json.loads(self.paths[0].read_text())
        log = self.paths[0].parent / Path(receipt["checks"][0]["log"]).name
        original = log.read_bytes()
        log.unlink()
        with self.assertRaises(verifier.VerificationError):
            self.verify()
        log.write_bytes(original + b"modified")
        with self.assertRaises(verifier.VerificationError):
            self.verify()

    def test_cli_missing_artifact_replaces_old_success_with_failed_result(self):
        output = self.root / "aggregate.json"
        output.write_text('{"status":"passed"}')
        self.paths[-1].unlink()
        with patch("sys.argv", ["verify_host_shards.py", str(self.root), "--expected-head", HEAD,
                                "--expected-run-id", EXECUTION["run_id"],
                                "--expected-run-attempt", EXECUTION["run_attempt"],
                                "--output", str(output)]), \
             patch.object(runner, "source_head", return_value=HEAD), \
             patch.object(runner, "source_clean", return_value=True), \
             patch.object(build_unified, "inputs", return_value=self.inputs), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(verifier.main(), 1)
        report = json.loads(output.read_text())
        self.assertEqual(report["status"], "failed")
        self.assertIn("exactly four", report["error"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
