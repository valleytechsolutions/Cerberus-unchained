"""Exercise build reporting failure paths without a toolchain or hardware."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import check_mesh_locate_iram
import check_c5_raw_link

import build_unified as builder
import package_sentinel as packager


class BuildReportChecks(unittest.TestCase):
    def run_build(self, exit_code, outputs, target="example"):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / ".pio/build/example"
            build.mkdir(parents=True)
            for name in outputs:
                (build / name).write_bytes(b"test output")
            with patch.object(builder, "ROOT", root), \
                 patch.object(builder, "TARGETS", {target: (root, "example")}), \
                 patch("sys.argv", ["build_unified.py"]), \
                 patch.object(builder.subprocess, "run", return_value=SimpleNamespace(returncode=exit_code)) as run, \
                 contextlib.redirect_stdout(io.StringIO()):
                code = builder.main()
                command = run.call_args.args[0]
                self.assertNotIn("upload", command)
                self.assertNotIn("erase", command)
            report = json.loads((root / "artifacts/integration/build-report.json").read_text())
            return code, report

    def test_failed_build_does_not_report_stale_binaries(self):
        code, report = self.run_build(1, ["firmware.bin", "firmware.elf", "partitions.bin"])
        self.assertEqual(code, 1)
        self.assertEqual(report["status"], "failed")
        self.assertNotIn("files", report["targets"]["example"])

    def test_success_without_required_images_is_failure(self):
        code, report = self.run_build(0, ["firmware.bin"])
        self.assertEqual(code, 1)
        self.assertEqual(report["targets"]["example"]["missing_outputs"], ["firmware.elf", "partitions.bin"])

    def test_complete_build_records_checksums_without_hardware_claim(self):
        code, report = self.run_build(0, ["firmware.bin", "firmware.elf", "partitions.bin"])
        self.assertEqual(code, 0)
        self.assertEqual(report["status"], "passed")
        self.assertFalse(report["hardware_validated"])
        self.assertEqual(len(report["targets"]["example"]["files"]["firmware.bin"]["sha256"]), 64)

    def test_c5_locate_native_gate_failure_fails_build(self):
        with patch.object(check_mesh_locate_iram,"verify",side_effect=RuntimeError("callback in flash")) as gate, \
             patch.object(check_c5_raw_link,"verify",return_value={"status":"passed"}):
            code,report=self.run_build(0,["firmware.bin","firmware.elf","partitions.bin"],"onboard")
        gate.assert_called_once()
        self.assertEqual(code,1)
        self.assertEqual(report["targets"]["onboard"]["mesh_locate_iram_error"],"callback in flash")

    def test_c5_locate_native_gate_evidence_is_retained(self):
        proof={"status":"passed","hardware_validated":False,"elf_sha256":"sample"}
        with patch.object(check_mesh_locate_iram,"verify",return_value=proof) as gate, \
             patch.object(check_c5_raw_link,"verify",return_value={"status":"passed"}):
            code,report=self.run_build(0,["firmware.bin","firmware.elf","partitions.bin"],"onboard")
        gate.assert_called_once()
        self.assertEqual(code,0)
        self.assertEqual(report["targets"]["onboard"]["mesh_locate_iram"],proof)


class PackageSourceChecks(unittest.TestCase):
    def test_vendored_source_changes_require_a_new_matched_build(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            vendor = root / "third_party/esp_crt_bundle"
            vendor.mkdir(parents=True)
            source = vendor / "esp_crt_bundle.c"
            source.write_text("original handler", encoding="utf-8")
            (vendor / "CMakeLists.txt").write_text("build hook", encoding="utf-8")
            receipt_path = root / "matched-build.json"
            with patch.object(packager, "ROOT", root), \
                 patch.object(packager, "TARGETS", {}), \
                 patch.object(packager, "RECEIPT", receipt_path), \
                 patch.object(packager, "validate_console_proof"), \
                 patch.object(packager, "validate_locate_proof"), \
                 patch.object(packager, "inspect_target") as inspect:
                before = packager.source_snapshot()
                self.assertIn("third_party/esp_crt_bundle/esp_crt_bundle.c", before["files"])
                self.assertIn("third_party/esp_crt_bundle/CMakeLists.txt", before["files"])
                receipt_path.write_text(json.dumps({"source": before}), encoding="utf-8")
                source.write_text("changed handler", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "Build inputs changed"):
                    packager.package(root / "package")
                inspect.assert_not_called()
                self.assertFalse((root / "package").exists())


class MatchedConsoleProofChecks(unittest.TestCase):
    """The standalone packager must reject stale or missing native evidence."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        patcher = patch.object(packager, "ROOT", self.root)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.bound = (".pio/build/s3-main/firmware.elf", "s3_main/main/usb_console.c", "tools/build_unified.py")
        for name in self.bound:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode("utf-8"))
        self.proof = {
            "status": "passed", "hardware_validated": False,
            "sha256": packager.sha256(self.root / self.bound[0]),
            "source_sha256": packager.sha256(self.root / self.bound[1]),
            "checker_sha256": packager.sha256(self.root / self.bound[2]),
            "allocated_stack_bytes": 4096, "task_frame_limit_bytes": 192,
            "formatter_frame_limit_bytes": 128,
            "frames_bytes": {"usb_console_task": 112, "console_printf": 80}, "known_frame_bytes": 2848,
            "remaining_after_known_frames_bytes": 1248,
            "minimum_remaining_after_known_frames_bytes": 1024,
        }
        self.json_path = self.root / "artifacts/integration/s3-console-stack.json"
        self.json_path.parent.mkdir(parents=True, exist_ok=True)
        self.json_path.write_text(json.dumps(self.proof), encoding="utf-8")
        self.text_path = self.json_path.with_suffix(".txt")
        self.text_path.write_text("synthetic debugger evidence", encoding="utf-8")
        self.receipt = {"native_console_stack": self.proof,
            "native_console_evidence": {path.relative_to(self.root).as_posix(): packager.sha256(path)
                for path in (self.json_path, self.text_path)}}

    def test_matching_final_elf_and_evidence_are_accepted(self):
        self.assertEqual(packager.validate_console_proof(self.receipt), self.proof)

    def test_legacy_receipt_without_native_proof_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Missing native console proof"):
            packager.validate_console_proof({})

    def test_each_bound_input_change_rejects_the_old_proof(self):
        for name in self.bound:
            with self.subTest(path=name):
                path = self.root / name
                original = path.read_bytes()
                path.write_bytes(original + b" changed")
                with self.assertRaisesRegex(ValueError, "proof is stale"):
                    packager.validate_console_proof(self.receipt)
                path.write_bytes(original)

    def test_changed_debugger_evidence_is_rejected(self):
        self.text_path.write_text("different output", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "evidence changed"):
            packager.validate_console_proof(self.receipt)

    def test_invalid_native_budget_cannot_be_packaged(self):
        self.proof["known_frame_bytes"] = 4096
        self.proof["remaining_after_known_frames_bytes"] = 0
        with self.assertRaisesRegex(ValueError, "frame budget"):
            packager.validate_console_proof(self.receipt)

    def test_large_or_missing_formatter_frame_cannot_be_packaged(self):
        for frame in (0, 144, 848):
            with self.subTest(frame=frame):
                self.proof["frames_bytes"]["console_printf"] = frame
                with self.assertRaisesRegex(ValueError, "frame budget"):
                    packager.validate_console_proof(self.receipt)
        self.proof["frames_bytes"]["console_printf"] = 80
        del self.proof["formatter_frame_limit_bytes"]
        with self.assertRaisesRegex(ValueError, "frame budget"):
            packager.validate_console_proof(self.receipt)


class MatchedLocateProofChecks(unittest.TestCase):
    """Exercise exact final-image/source/evidence binding without native tools."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        patcher = patch.object(packager, "ROOT", self.root)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.elf = "c5_firmware/.pio/build/c5-onboard/firmware.elf"
        self.checker = "tools/check_mesh_locate_iram.py"
        self.sources = (
            "shared/mesh_mac.c", "shared/mesh_mac.h", "shared/mesh_locate_protocol.h",
            "c5_firmware/main/ieee802154_locate_impl.h",
            "c5_firmware/main/ieee802154_scanner.c", "c5_firmware/main/CMakeLists.txt",
        )
        for name in (self.elf, self.checker, *self.sources):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode("utf-8"))
        self.proof = {
            "status": "passed", "hardware_validated": False,
            "elf_sha256": packager.sha256(self.root / self.elf),
            "checker_sha256": packager.sha256(self.root / self.checker),
            "source_hashes": {name: packager.sha256(self.root / name) for name in self.sources},
            "required_roots": ["mesh_mac_parse", "locate_receive"],
            "limits": "Synthetic binding fixture; no native placement or hardware claim.",
        }
        self.json_path = self.root / "artifacts/integration/c5-mesh-locate-iram.json"
        self.json_path.parent.mkdir(parents=True, exist_ok=True)
        self.json_path.write_text(json.dumps(self.proof), encoding="utf-8")
        self.text_path = self.json_path.with_suffix(".txt")
        self.text_path.write_text("synthetic disassembly evidence", encoding="utf-8")
        self.receipt = {
            "native_mesh_locate_iram": self.proof,
            "native_mesh_locate_evidence": {
                path.relative_to(self.root).as_posix(): packager.sha256(path)
                for path in (self.json_path, self.text_path)
            },
        }

    def test_matching_final_elf_sources_and_evidence_are_accepted(self):
        self.assertEqual(packager.validate_locate_proof(self.receipt), self.proof)

    def test_missing_failed_or_hardware_claiming_proof_is_rejected(self):
        for proof in (None, {}, {**self.proof, "status": "failed"},
                      {**self.proof, "hardware_validated": True}):
            with self.subTest(proof=proof), self.assertRaisesRegex(ValueError, "Missing native Mesh Locate proof"):
                packager.validate_locate_proof({**self.receipt, "native_mesh_locate_iram": proof})

    def test_changed_final_elf_or_checker_rejects_the_old_proof(self):
        for name in (self.elf, self.checker):
            with self.subTest(path=name):
                path = self.root / name
                original = path.read_bytes()
                path.write_bytes(original + b" changed")
                with self.assertRaisesRegex(ValueError, "proof is stale"):
                    packager.validate_locate_proof(self.receipt)
                path.write_bytes(original)

    def test_each_changed_source_rejects_the_old_proof(self):
        for name in self.sources:
            with self.subTest(path=name):
                path = self.root / name
                original = path.read_bytes()
                path.write_bytes(original + b" changed")
                with self.assertRaisesRegex(ValueError, "source proof is stale"):
                    packager.validate_locate_proof(self.receipt)
                path.write_bytes(original)

    def test_missing_or_extra_source_mapping_is_rejected(self):
        sources = self.proof["source_hashes"]
        for changed in ({name: value for name, value in sources.items() if name != self.sources[0]},
                        {**sources, "unreviewed.c": "0" * 64}):
            with self.subTest(sources=changed), self.assertRaisesRegex(ValueError, "source proof is stale"):
                packager.validate_locate_proof({**self.receipt,
                    "native_mesh_locate_iram": {**self.proof, "source_hashes": changed}})

    def test_missing_or_extra_evidence_mapping_is_rejected(self):
        evidence = self.receipt["native_mesh_locate_evidence"]
        for changed in ({}, {**evidence, "unreviewed.txt": "0" * 64},
                        {name: value for name, value in evidence.items() if name.endswith(".json")}):
            with self.subTest(evidence=changed), self.assertRaisesRegex(ValueError, "evidence changed or is missing"):
                packager.validate_locate_proof({**self.receipt, "native_mesh_locate_evidence": changed})

    def test_changed_json_or_disassembly_evidence_is_rejected(self):
        for path in (self.json_path, self.text_path):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b" changed")
                with self.assertRaisesRegex(ValueError, "evidence changed or is missing"):
                    packager.validate_locate_proof(self.receipt)
                path.write_bytes(original)

    def test_rehashed_final_json_must_still_match_receipt_proof(self):
        changed = {**self.proof, "limits": "Different final evidence"}
        self.json_path.write_text(json.dumps(changed), encoding="utf-8")
        self.receipt["native_mesh_locate_evidence"][self.json_path.relative_to(self.root).as_posix()] = packager.sha256(self.json_path)
        with self.assertRaisesRegex(ValueError, "JSON differs from the matched receipt"):
            packager.validate_locate_proof(self.receipt)

    def test_deleted_bound_files_fail_closed(self):
        for name in (self.elf, self.checker, *self.sources,
                     *self.receipt["native_mesh_locate_evidence"]):
            with self.subTest(path=name):
                path = self.root / name
                original = path.read_bytes()
                path.unlink()
                with self.assertRaises(OSError):
                    packager.validate_locate_proof(self.receipt)
                path.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
