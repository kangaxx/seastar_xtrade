#!/usr/bin/env python3
#
# Licensed under the Apache License, Version 2.0. See LICENSE and NOTICE.
# B7/B8 offline fixtures; not business acceptance evidence.
# Source: development plan B5-B8, 2026-11-10.
# Signature: GitHub Copilot (gpt-6-astra).

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "xtrader_acceptance", Path(__file__).resolve().parents[2] / "tools" / "xtrader_acceptance.py"
)
acceptance = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(acceptance)


def ledger():
    scope = {field: "synthetic" for field in acceptance.SCOPE_FIELDS}
    scope["trading_day"] = "20260102"
    records = []
    for sequence, kind in enumerate(acceptance.KINDS, 1):
        values = {
            field: 0 if field in acceptance.INTEGER_FIELDS else "synthetic"
            for field in acceptance.VALUE_FIELDS[kind]
        }
        if kind in ("order", "trade"):
            values.update(side="buy", offset="open", volume=1, hedge="speculation")
        if kind == "order":
            values.update(status="accepted", type="limit", limit_price_ticks=100)
        if kind == "position":
            values["hedge"] = "speculation"
        if kind == "trade":
            values.update(price_ticks=100, is_approximate=True, fill_source="simulated_best_quote")
        records.append({
            "schema_version": 1, "scope": dict(scope), "sequence": sequence, "event_time_ns": sequence,
            "kind": kind, "key": f"key-{kind}", "values": values,
        })
    return records


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def write_json(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def write_records(self, name, records):
        path = self.root / name
        path.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")
        return path

    def complete_manifest(self):
        artifact = self.root / "synthetic-evidence.txt"
        artifact.write_bytes(b"synthetic fixture only")
        manifest = acceptance.template()
        manifest["implementation_revision"] = "synthetic-revision"
        manifest["artifacts"] = {
            "fixture": {"path": artifact.name, "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest()}
        }
        passed = {"status": "passed", "evidence": ["fixture"], "review": "synthetic-review"}
        for group in ("wbs", "modes"):
            for key in manifest[group]:
                manifest[group][key] = dict(passed)
        for field in ("comparison", "single_writer_rehearsal", "rollback_rehearsal"):
            manifest[field] = dict(passed)
        manifest["open_blockers"] = []
        manifest["environments"] = [{
            "id": name, "business_chain": True, "soak_8h_seconds": 28800,
            "soak_24h_seconds": 86400, "lost_orders": 0, "lost_trades": 0,
            "capacity_passed": True, "recovery_passed": True, "resource_trends_reviewed": True,
            "threshold_approval": "fixture", "evidence": ["fixture"], "review": "synthetic-review",
        } for name in ("synthetic-a", "synthetic-b")]
        return manifest

    def test_template_remains_blocked(self):
        result = acceptance.check_manifest(self.write_json("manifest.json", acceptance.template()))
        self.assertEqual(result["status"], "blocked")
        self.assertFalse(result["live_authorized"])

    def test_complete_inventory_never_authorizes_trading(self):
        result = acceptance.check_manifest(self.write_json("manifest.json", self.complete_manifest()))
        self.assertEqual(result["status"], "evidence_inventory_complete")
        self.assertFalse(result["live_authorized"])

    def test_tampering_and_path_escape_block_inventory(self):
        for path in ("../outside.txt", "missing.txt", "synthetic-evidence.txt"):
            manifest = self.complete_manifest()
            manifest["artifacts"]["fixture"].update(path=path, sha256="0" * 64)
            result = acceptance.check_manifest(self.write_json("manifest.json", manifest))
            self.assertEqual(result["status"], "blocked")

    def test_boolean_duration_and_order_loss_do_not_pass(self):
        manifest = self.complete_manifest()
        manifest["environments"][0]["soak_8h_seconds"] = True
        manifest["environments"][1]["lost_orders"] = 1
        result = acceptance.check_manifest(self.write_json("manifest.json", manifest))
        self.assertEqual(result["status"], "blocked")

    def test_missing_wbs_and_duplicate_environment_are_invalid(self):
        manifest = self.complete_manifest()
        del manifest["wbs"]["R3-05"]
        with self.assertRaises(ValueError):
            acceptance.check_manifest(self.write_json("manifest.json", manifest))
        manifest = self.complete_manifest()
        manifest["environments"][1]["id"] = manifest["environments"][0]["id"]
        with self.assertRaises(ValueError):
            acceptance.check_manifest(self.write_json("manifest.json", manifest))

    def test_identical_and_one_unit_account_difference(self):
        expected = self.write_records("expected.jsonl", ledger())
        with self.assertRaises(ValueError):
            acceptance.compare_ledgers(expected, expected)
        actual = self.write_records("actual.jsonl", ledger())
        self.assertEqual(acceptance.compare_ledgers(expected, actual)["status"], "identical")
        changed = ledger()
        changed[-1]["values"]["balance"] += 1
        actual = self.write_records("actual.jsonl", changed)
        result = acceptance.compare_ledgers(expected, actual)
        self.assertEqual(result["status"], "different")
        self.assertEqual(result["difference_count"], 1)

    def test_empty_truncated_duplicate_sequence_and_float_are_rejected(self):
        expected = self.write_records("expected.jsonl", ledger())
        for records in ([], ledger()[:-1]):
            actual = self.write_records("actual.jsonl", records)
            self.assertEqual(acceptance.compare_ledgers(expected, actual)["status"], "different")
        changed = ledger()
        changed[1]["sequence"] = 1
        with self.assertRaises(ValueError):
            acceptance.compare_ledgers(expected, self.write_records("actual.jsonl", changed))
        changed = ledger()
        changed[-1]["values"]["balance"] = 0.0
        with self.assertRaises(ValueError):
            acceptance.compare_ledgers(expected, self.write_records("actual.jsonl", changed))

    def test_duplicate_keys_and_nonfinite_json_are_invalid(self):
        for raw in ('{"status": "passed", "status": "pending"}', '{"value": NaN}'):
            with self.assertRaises(ValueError):
                acceptance.parse_json(raw)

    def test_cross_day_exports_and_duplicate_settlement(self):
        records = ledger()
        next_day = ledger()
        for record in next_day:
            record["sequence"] += 4
            record["event_time_ns"] += 4
            record["scope"]["trading_day"] = "20260103"
        records.extend(next_day)
        expected = self.write_records("expected.jsonl", records)
        actual = self.write_records("actual.jsonl", records)
        self.assertEqual(acceptance.compare_ledgers(expected, actual)["status"], "identical")
        records[-3]["scope"]["trading_day"] = "20260102"
        with self.assertRaises(ValueError):
            acceptance.compare_ledgers(expected, self.write_records("actual.jsonl", records))
        records = ledger()
        duplicate = dict(records[1], sequence=5, event_time_ns=5)
        records.append(duplicate)
        with self.assertRaises(ValueError):
            acceptance.compare_ledgers(expected, self.write_records("actual.jsonl", records))


if __name__ == "__main__":
    unittest.main()
