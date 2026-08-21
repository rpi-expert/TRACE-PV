#!/usr/bin/env python3
"""Summarize one mission-profile iteration and project the first failure cycle.

This is a portable post-processor for a stressor CSV emitted by trace_pv.  It
does not replace the CUDA electrical/thermal simulation that creates that CSV.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


DAMAGE_MODES = (
    ("fan_electrical_external", "Fan cooling — electrical, external temperature"),
    ("fan_electrical_internal", "Fan cooling — electrical, internal temperature"),
    ("fan_mechanical_external", "Fan cooling — mechanical, external temperature"),
    ("fan_mechanical_internal", "Fan cooling — mechanical, internal temperature"),
    ("capacitor", "Capacitor"),
    ("igbt_deltaT", "IGBT — delta-T cycling"),
    ("igbt_arrhenius", "IGBT — Arrhenius"),
    ("pcb", "PCB"),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stressor_csv", type=Path, help="One-year trace_pv stressor CSV")
    parser.add_argument("output_dir", type=Path, help="Directory for summary artifacts")
    parser.add_argument("--igbt-arrhenius-n2", type=float, required=True)
    parser.add_argument("--source-run", default="", help="Provenance label for the CUDA run")
    parser.add_argument(
        "--execution-note",
        default="",
        help="Qualification such as whether the CUDA stage was run on this host",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    totals = {column: 0.0 for column, _ in DAMAGE_MODES}
    rows = 0

    with args.stressor_csv.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        missing = [column for column in totals if column not in (reader.fieldnames or [])]
        if missing:
            raise ValueError(f"Missing stressor columns: {', '.join(missing)}")
        for record in reader:
            rows += 1
            for column in totals:
                totals[column] += float(record[column])

    if rows == 0:
        raise ValueError("The stressor CSV contains no data rows")

    results = []
    for column, label in DAMAGE_MODES:
        damage = totals[column]
        projected_years = None if damage <= 0.0 else 1.0 / damage
        failure_cycle = None if projected_years is None else math.ceil(projected_years)
        results.append(
            {
                "damage_mode": column,
                "label": label,
                "one_year_damage": damage,
                "one_year_remaining_margin": 1.0 - damage,
                "projected_life_years": projected_years,
                "first_full_mission_cycle_at_or_above_one": failure_cycle,
            }
        )

    ranked = sorted(
        results,
        key=lambda item: (
            math.inf if item["projected_life_years"] is None else item["projected_life_years"]
        ),
    )
    first = ranked[0]
    failure_cycle = first["first_full_mission_cycle_at_or_above_one"]
    for item in results:
        item["damage_at_first_failure_cycle"] = (
            item["one_year_damage"] * failure_cycle if failure_cycle is not None else None
        )

    summary = {
        "analysis_type": "one-year stressor aggregation and repeated-profile lifetime cycle",
        "source_stressor_csv": str(args.stressor_csv),
        "source_run": args.source_run,
        "execution_note": args.execution_note,
        "mission_profile_cases": rows,
        "igbt_arrhenius_n2": args.igbt_arrhenius_n2,
        "failure_threshold": 1.0,
        "first_failed_damage_mode": first["damage_mode"],
        "first_failed_component": first["label"],
        "projected_first_failure_years": first["projected_life_years"],
        "first_full_mission_cycle_with_failure": failure_cycle,
        "damage_modes": results,
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.output_dir / "lifetime_cycle_summary.json"
    csv_path = args.output_dir / "one_year_damage_and_lifetime.csv"
    report_path = args.output_dir / "REPORT.md"

    with json_path.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, allow_nan=False)
        handle.write("\n")

    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        fieldnames = list(results[0])
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(results)

    report_lines = [
        "# TRACE-PV one-year and lifetime-cycle result",
        "",
        f"- Source stressor CSV: `{args.stressor_csv}`",
        f"- Source run: {args.source_run or 'not specified'}",
        f"- Execution note: {args.execution_note or 'not specified'}",
        f"- Mission-profile cases: {rows}",
        f"- IGBT Arrhenius `n2`: {args.igbt_arrhenius_n2:g}",
        f"- First failed component/mode: **{first['label']}**",
        f"- Continuous projected life: **{first['projected_life_years']:.6f} years**",
        f"- First completed annual cycle at/above damage 1.0: **{failure_cycle}**",
        "",
        "| Damage mode | One-year damage | Projected life (years) | Damage after first-failure cycle |",
        "|---|---:|---:|---:|",
    ]
    for item in ranked:
        years = item["projected_life_years"]
        years_text = "infinite" if years is None else f"{years:.6f}"
        report_lines.append(
            f"| {item['label']} | {item['one_year_damage']:.9g} | {years_text} | "
            f"{item['damage_at_first_failure_cycle']:.9g} |"
        )
    report_path.write_text("\n".join(report_lines) + "\n", encoding="utf-8")

    print(json.dumps(summary, indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
