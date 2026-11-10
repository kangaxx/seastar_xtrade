#!/usr/bin/env python3
#
# Licensed under the Apache License, Version 2.0. See LICENSE and NOTICE.
# B7/B8: offline evidence inventory and exact normalized ledger comparison.
# Source: development plan B5-B8, 2026-11-10.
# Signature: GitHub Copilot (gpt-6-astra).
"""Read-only tools. A successful result never authorizes or enables trading."""

import argparse
from datetime import datetime
import hashlib
import itertools
import json
from pathlib import Path
import re
import sys


WBS = tuple(
    f"R{phase}-{item:02d}"
    for phase, count in ((1, 2), (2, 3), (3, 5), (4, 8), (5, 4), (6, 5))
    for item in range(1, count + 1)
)
MODES = ("replay", "paper", "ctp_sim", "live_disabled")
KINDS = ("order", "trade", "position", "account")
SCOPE_FIELDS = (
    "dataset_id", "parameter_set_id", "trading_day", "account_alias",
    "strategy_id", "money_unit", "price_unit", "time_domain",
)
VALUE_FIELDS = {
    "order": ("exchange", "instrument", "side", "offset", "hedge", "type",
              "limit_price_ticks", "volume", "filled", "status"),
    "trade": ("exchange", "instrument", "side", "offset", "hedge", "order_key",
              "price_ticks", "volume", "commission_units", "margin_change_units",
              "close_profit_units", "is_approximate", "fill_source"),
    "position": ("exchange", "instrument", "hedge", "long_today", "long_yesterday",
                 "short_today", "short_yesterday", "frozen_long_today", "frozen_long_yesterday",
                 "frozen_short_today", "frozen_short_yesterday"),
    "account": ("balance", "available", "margin", "frozen_margin", "frozen_commission",
                "commission", "close_profit"),
}
INTEGER_FIELDS = {
    "volume", "filled", "price_ticks", "limit_price_ticks", "commission_units",
    "margin_change_units", "close_profit_units", "long_today",
    "long_yesterday", "short_today", "short_yesterday", "frozen_long_today",
    "frozen_long_yesterday", "frozen_short_today", "frozen_short_yesterday",
    "balance", "available", "margin", "frozen_margin", "frozen_commission",
    "commission", "close_profit",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def text(value):
    return isinstance(value, str) and bool(value.strip())


def integer(value, minimum=0):
    return type(value) is int and value >= minimum


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError(f"non-finite JSON value: {value}")


def parse_json(raw):
    return json.loads(raw, object_pairs_hook=unique_object, parse_constant=reject_constant)


def check_keys(value, expected, label):
    require(isinstance(value, dict) and set(value) == set(expected), f"{label}: wrong fields")


def template():
    pending = {"status": "pending", "evidence": [], "review": ""}
    return {
        "schema_version": 1,
        "implementation_revision": "",
        "artifacts": {},
        "wbs": {item: dict(pending) for item in WBS},
        "modes": {mode: dict(pending) for mode in MODES},
        "environments": [],
        "comparison": dict(pending),
        "single_writer_rehearsal": dict(pending),
        "rollback_rehearsal": dict(pending),
        "open_blockers": ["B1-B8 implementation and acceptance remain incomplete"],
    }


def artifact_inventory(artifacts, root):
    require(isinstance(artifacts, dict), "artifacts must be an object")
    valid = set()
    errors = []
    for name, artifact in artifacts.items():
        try:
            require(text(name), "artifact ID must be nonempty")
            check_keys(artifact, ("path", "sha256"), f"artifact {name}")
            require(text(artifact["path"]), f"artifact {name}: empty path")
            require(isinstance(artifact["sha256"], str)
                    and re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"]),
                    f"artifact {name}: invalid SHA-256")
            relative = Path(artifact["path"])
            require(not relative.is_absolute(), f"artifact {name}: absolute path")
            path = (root / relative).resolve()
            require(root in path.parents, f"artifact {name}: escapes manifest directory")
            require(path.is_file() and path.stat().st_size > 0, f"artifact {name}: missing/empty file")
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            require(digest.hexdigest() == artifact["sha256"], f"artifact {name}: SHA-256 mismatch")
            valid.add(name)
        except (ValueError, OSError) as error:
            errors.append(str(error))
    return valid, errors


def check_evidence_refs(refs, valid, label, errors):
    if (not isinstance(refs, list) or not refs
            or any(not text(ref) or ref not in valid for ref in refs)
            or len(set(refs)) != len(refs)):
        errors.append(f"{label}: missing, duplicate or unverified evidence")


def check_result(record, valid, label, errors):
    check_keys(record, ("status", "evidence", "review"), label)
    if record["status"] != "passed":
        errors.append(f"{label}: not passed")
    if not text(record["review"]):
        errors.append(f"{label}: no review record")
    check_evidence_refs(record["evidence"], valid, label, errors)


def check_environment(environment, valid, errors):
    fields = ("id", "business_chain", "soak_8h_seconds", "soak_24h_seconds",
              "lost_orders", "lost_trades", "capacity_passed", "recovery_passed",
              "resource_trends_reviewed", "threshold_approval", "evidence", "review")
    check_keys(environment, fields, "environment")
    label = f"environment {environment['id']}"
    require(text(environment["id"]), "environment ID is required")
    for name in ("business_chain", "capacity_passed", "recovery_passed", "resource_trends_reviewed"):
        if environment[name] is not True:
            errors.append(f"{label}: {name} is not confirmed")
    for name, minimum in (("soak_8h_seconds", 8 * 3600), ("soak_24h_seconds", 24 * 3600)):
        if not integer(environment[name], minimum):
            errors.append(f"{label}: insufficient {name}")
    for name in ("lost_orders", "lost_trades"):
        if type(environment[name]) is not int or environment[name] != 0:
            errors.append(f"{label}: {name} must be zero")
    check_evidence_refs(environment["evidence"], valid, label, errors)
    check_evidence_refs([environment["threshold_approval"]], valid, f"{label} thresholds", errors)
    if not text(environment["review"]):
        errors.append(f"{label}: no review record")


def check_manifest(path):
    path = Path(path).resolve()
    manifest = parse_json(path.read_text(encoding="utf-8-sig"))
    check_keys(manifest, template(), "manifest")
    require(type(manifest["schema_version"]) is int and manifest["schema_version"] == 1,
            "unsupported manifest schema")
    valid, errors = artifact_inventory(manifest["artifacts"], path.parent)
    if not text(manifest["implementation_revision"]):
        errors.append("implementation_revision is missing")
    for group, keys in (("wbs", WBS), ("modes", MODES)):
        check_keys(manifest[group], keys, group)
        for key in keys:
            check_result(manifest[group][key], valid, key, errors)
    environments = manifest["environments"]
    require(isinstance(environments, list), "environments must be a list")
    seen = set()
    for environment in environments:
        check_environment(environment, valid, errors)
        require(environment["id"] not in seen, "duplicate environment ID")
        seen.add(environment["id"])
    if len(seen) < 2:
        errors.append("at least two distinct target environments are required")
    for field in ("comparison", "single_writer_rehearsal", "rollback_rehearsal"):
        check_result(manifest[field], valid, field, errors)
    require(isinstance(manifest["open_blockers"], list)
            and all(text(blocker) for blocker in manifest["open_blockers"]),
            "open_blockers must be a list of nonempty descriptions")
    if manifest["open_blockers"]:
        errors.append("open blockers remain")
    return {
        "status": "blocked" if errors else "evidence_inventory_complete",
        "live_authorized": False,
        "errors": errors,
        "verified_artifact_count": len(valid),
        "notice": "Hashes verify files, not their claims. Human review and separate trading approval remain mandatory.",
    }


def check_value_types(values, kind, label):
    check_keys(values, VALUE_FIELDS[kind], label)
    for key, value in values.items():
        if key in INTEGER_FIELDS:
            require(type(value) is int, f"{label}.{key}: integer units required")
            if key not in ("balance", "available", "close_profit", "margin_change_units", "close_profit_units"):
                require(value >= 0, f"{label}.{key}: negative value")
        elif key == "is_approximate":
            require(type(value) is bool, f"{label}.{key}: boolean required")
        else:
            require(text(value), f"{label}.{key}: nonempty text required")
    if kind in ("order", "trade"):
        require(values["side"] in ("buy", "sell"), f"{label}: invalid side")
        require(values["offset"] in ("open", "close", "close_today", "close_yesterday"),
                f"{label}: invalid offset")
        require(values["volume"] > 0, f"{label}: nonpositive volume")
    if kind != "account":
        require(values["hedge"] in ("speculation", "hedge", "arbitrage"), f"{label}: invalid hedge")
    if kind == "order":
        require(values["filled"] <= values["volume"], f"{label}: overfilled order")
        require(values["type"] in ("limit", "market", "fak", "fok"), f"{label}: invalid order type")
        require(values["type"] == "market" or values["limit_price_ticks"] > 0,
                f"{label}: missing limit price")
        require(values["status"] in ("submitted", "accepted", "partially_filled", "filled",
                                     "canceled", "rejected", "unknown", "not_touched", "touched"),
                f"{label}: invalid order status")
    if kind == "trade":
        require(values["price_ticks"] > 0, f"{label}: nonpositive price")
        require(values["fill_source"] in ("ctp_report", "simulated_best_quote", "simulated_bar_open"),
                f"{label}: unknown fill source")
        require(values["fill_source"] == "ctp_report" or values["is_approximate"],
                f"{label}: simulated fill must be approximate")


def read_records(path):
    with Path(path).open("r", encoding="utf-8-sig") as stream:
        for sequence, line in enumerate(stream, 1):
            require(len(line) <= 1024 * 1024, f"{path}:{sequence}: record too large")
            record = parse_json(line)
            label = f"{path}:{sequence}"
            check_keys(record, ("schema_version", "scope", "sequence", "event_time_ns", "kind", "key", "values"), label)
            require(type(record["schema_version"]) is int and record["schema_version"] == 1,
                    f"{label}: unsupported schema")
            require(type(record["sequence"]) is int and record["sequence"] == sequence,
                    f"{label}: sequence must be contiguous from 1")
            require(integer(record["event_time_ns"]), f"{label}: integer nonnegative event_time_ns required")
            check_keys(record["scope"], SCOPE_FIELDS, f"{label}.scope")
            require(all(text(value) for value in record["scope"].values()), f"{label}: empty scope")
            day = record["scope"]["trading_day"]
            require(re.fullmatch(r"[0-9]{8}", day), f"{label}: trading_day must be YYYYMMDD")
            datetime.strptime(day, "%Y%m%d")
            require(isinstance(record["kind"], str) and record["kind"] in KINDS, f"{label}: invalid kind")
            require(text(record["key"]), f"{label}: missing stable business key")
            check_value_types(record["values"], record["kind"], label)
            yield record


def compare_ledgers(expected, actual):
    require(not Path(expected).samefile(actual), "comparison requires two distinct export files")
    differences = []
    difference_count = 0
    counts = {"expected": 0, "actual": 0}
    kinds = {"expected": set(), "actual": set()}
    scopes = {}
    days = {}
    timestamps = {}
    trade_keys = {"expected": set(), "actual": set()}
    for sequence, pair in enumerate(itertools.zip_longest(read_records(expected), read_records(actual)), 1):
        for side, record in zip(("expected", "actual"), pair):
            if record is not None:
                counts[side] += 1
                kinds[side].add(record["kind"])
                scope = {key: value for key, value in record["scope"].items() if key != "trading_day"}
                day = record["scope"]["trading_day"]
                if side not in scopes:
                    scopes[side] = scope
                require(scopes[side] == scope, f"{side}:{sequence}: scope changed within export")
                require(side not in days or day >= days[side], f"{side}:{sequence}: trading day regressed")
                days[side] = day
                timestamp = record["event_time_ns"]
                require(side not in timestamps or timestamp >= timestamps[side],
                        f"{side}:{sequence}: observation time regressed")
                timestamps[side] = timestamp
                if record["kind"] == "trade":
                    key = (day, record["values"]["exchange"], record["key"])
                    require(key not in trade_keys[side], f"{side}:{sequence}: duplicate settled trade")
                    trade_keys[side].add(key)
        if pair[0] != pair[1]:
            difference_count += 1
            if len(differences) < 20:
                # Do not echo account values or whole records to terminal output.
                differences.append({"sequence": sequence, "reason": "missing or unequal normalized record"})
    missing = {side: sorted(set(KINDS) - seen) for side, seen in kinds.items() if set(KINDS) - seen}
    return {
        "status": "different" if difference_count or missing else "identical",
        "counts": counts,
        "difference_count": difference_count,
        "differences": differences,
        "missing_record_kinds": missing,
        "live_authorized": False,
        "notice": "Exact exported values only; not proof that either producer is correct.",
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("template", help="print a blocked evidence manifest; never fabricate evidence")
    gate = commands.add_parser("gate", help="check evidence inventory; does not execute tests")
    gate.add_argument("manifest", type=Path)
    compare = commands.add_parser("compare", help="compare normalized JSONL exports without executing systems")
    compare.add_argument("expected", type=Path)
    compare.add_argument("actual", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "template":
            result = template()
            exit_code = 0
        elif args.command == "gate":
            result = check_manifest(args.manifest)
            exit_code = 0 if result["status"] == "evidence_inventory_complete" else 1
        else:
            result = compare_ledgers(args.expected, args.actual)
            exit_code = 0 if result["status"] == "identical" else 1
    except (ValueError, OSError, UnicodeError) as error:
        result = {"status": "invalid_input", "live_authorized": False, "error": str(error)}
        exit_code = 2
    print(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
