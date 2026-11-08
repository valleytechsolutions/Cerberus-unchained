"""Independent file-level export checks; fixtures contain synthetic data only."""
import csv
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("wardrive_csv", ROOT / "tools/validate_wardrive_csv.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
METADATA = ["WigleWifi-1.6", "appRelease=test", "model=Cerberus", "release=test",
            "device=Cerberus", "display=test", "board=S3-C5", "brand=Cerberus",
            "star=Sol", "body=3", "subBody=0"]
WIFI = ["02:11:22:33:44:55", 'Cafe, "test"', "WPA2", "2026-01-01 12:00:00",
        "36", "5180", "-45", "40.1", "-70.2", "10.1", "3.2", "", "", "WIFI"]
BLE = ["C2:11:22:33:44:55", "Tag", "[BLE]", "2026-01-01 12:00:00",
       "0", "", "-60", "40.1", "-70.2", "10.1", "3.2", "", "76", "BLE"]


class Exports(unittest.TestCase):
    def check(self, rows, metadata=None, header=None, raw=None):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "capture.csv"
            if raw is None:
                text = io.StringIO(newline="")
                writer = csv.writer(text, lineterminator="\n")
                writer.writerow(METADATA if metadata is None else metadata)
                writer.writerow(module.COLUMNS if header is None else header)
                writer.writerows(rows)
                raw = text.getvalue().encode("utf-8")
            path.write_bytes(raw)
            result = module.validate(path, now=datetime(2026, 11, 4, tzinfo=timezone.utc))
            self.assertEqual(raw, path.read_bytes(), "Validator must be read-only")
            rendered = json.dumps(result)
            for private in (WIFI[0], WIFI[1], WIFI[7], WIFI[8], BLE[0]):
                self.assertNotIn(private, rendered)
            self.assertEqual(result["server_acceptance"], "NOT_TESTED")
            return result

    def test_mixed_repeated_rows(self):
        result = self.check([WIFI, WIFI, BLE])
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["observations"], 3)
        self.assertEqual(result["types"], {"WIFI": 2, "BLE": 1})

    def test_missing_accuracy_is_reported_not_fabricated(self):
        row = WIFI.copy(); row[9] = row[10] = ""
        result = self.check([row])
        self.assertEqual(result["status"], "PASS_WITH_WARNINGS")
        self.assertEqual(result["warnings"], {"missing_altitude_metres": 1, "missing_accuracy_metres": 1})

    def test_legacy_planet_metadata_is_not_silently_accepted(self):
        result = self.check([WIFI], metadata=METADATA[:8])
        self.assertIn("incomplete_device_or_planet_metadata", result["errors"])

    def test_device_fields_and_order(self):
        for metadata in (METADATA[:-1] + ["subBody=1"],
                         METADATA + ["star=Sol"],
                         METADATA[:2] + [METADATA[3], METADATA[2]] + METADATA[4:]):
            with self.subTest(metadata=metadata):
                self.assertEqual(self.check([WIFI], metadata=metadata)["status"], "FAIL")

    def test_unlocated_and_empty_files(self):
        self.assertEqual(self.check([], metadata=["# Cerberus unlocated v4; RX!=UTC"])["status"], "FAIL")
        self.assertEqual(self.check([])["status"], "FAIL")

    def test_incorrect_columns(self):
        self.assertIn("column_count", self.check([WIFI[:-1]])["errors"])
        self.assertIn("column_header", self.check([WIFI], header=module.COLUMNS[:-1])["errors"])

    def test_invalid_fields(self):
        mutations = ((0, "01:11:22:33:44:55"), (0, "00:00:00:00:00:00"),
                     (0, "bad"), (3, "2030-01-01 12:00:00"),
                     (3, "2026-02-30 12:00:00"), (4, "15"), (5, "2437"),
                     (6, "-200"), (7, "nan"), (7, "91"), (8, "181"),
                     (9, "inf"), (10, "-1"), (10, "0"), (12, "76"),
                     (1, "hidden\x00name"), (1, "x" * 33), (13, "OTHER"))
        for column, value in mutations:
            with self.subTest(column=column, value=value):
                row = WIFI.copy(); row[column] = value
                self.assertEqual(self.check([row])["status"], "FAIL")

    def test_ble_fields(self):
        for column, value in ((4, "6"), (5, "2402"), (12, "65535"), (12, "-1")):
            row = BLE.copy(); row[column] = value
            self.assertEqual(self.check([row])["status"], "FAIL")
        row = BLE.copy(); row[12] = "0"
        self.assertEqual(self.check([row])["status"], "PASS")

    def test_utf8_and_truncation(self):
        self.assertEqual(self.check([], raw=b"\xff\n")["status"], "FAIL")
        self.assertEqual(self.check([], raw=b'WigleWifi-1.6,"unterminated')["status"], "FAIL")
        row = WIFI.copy(); row[1] = "Caf\u00e9"
        self.assertEqual(self.check([row])["status"], "PASS")

    def test_error_output_stays_bounded(self):
        result = self.check([WIFI[:-1]] * 2000)
        self.assertEqual(result["errors"]["column_count"], 2000)
        self.assertEqual(len(result["error_samples"]), 16)


if __name__ == "__main__":
    if len(sys.argv) == 2:
        generated = module.validate(Path(sys.argv.pop()))
        assert not generated["errors"], generated
        assert generated["observations"] == 3 and generated["types"] == {"WIFI": 2, "BLE": 1}
        assert generated["warnings"] == {"missing_altitude_metres": 3, "missing_accuracy_metres": 3}
    unittest.main()
