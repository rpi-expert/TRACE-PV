#!/usr/bin/env python3
"""Build the complete component database from the tracked JSON sources."""

import argparse
import sqlite3
import subprocess
import sys
from pathlib import Path

from init_database import (
    COMPONENT_FOLDERS,
    create_tables,
    load_capacitor,
    load_fan_cooling,
    load_grid,
    load_pcb,
    load_power_module,
    load_pv_inverter,
)


DB_FILE = "component_parameters.db"
LOADERS = {
    "capacitor": load_capacitor,
    "fan_cooling": load_fan_cooling,
    "power_module": load_power_module,
    "pcb": load_pcb,
    "pv_inverter": load_pv_inverter,
    "grid": load_grid,
}


def load_component_sources(conn: sqlite3.Connection, script_dir: Path) -> int:
    """Load every tracked component JSON file and return the number loaded."""
    loaded = 0
    failures = []

    for component_type, folder in COMPONENT_FOLDERS.items():
        json_dir = script_dir / folder
        if not json_dir.exists():
            print(f"Warning: component directory not found: {json_dir}")
            continue

        print(f"\nLoading {component_type} components...")
        for json_file in sorted(json_dir.glob("*.json")):
            try:
                part_number = LOADERS[component_type](conn, json_file)
                print(f"  Loaded: {part_number} from {json_file.name}")
                loaded += 1
            except Exception as exc:
                failures.append((json_file, exc))
                print(f"  Error loading {json_file.name}: {exc}")

    if failures:
        names = ", ".join(path.name for path, _ in failures)
        raise RuntimeError(f"Failed to load {len(failures)} component files: {names}")

    return loaded


def generate_pv_performance(script_dir: Path, db_path: Path) -> None:
    """Generate PV performance maps using the tracked Python implementation."""
    generator = script_dir / "offline_trainning" / "offline_data_generator.py"
    pv_panel_dir = script_dir / "pv_panel"
    if not generator.is_file():
        raise FileNotFoundError(f"PV generator source not found: {generator}")

    print("\nGenerating PV performance maps...")
    subprocess.run(
        [sys.executable, str(generator), str(db_path), str(pv_panel_dir)],
        cwd=generator.parent,
        check=True,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build component_parameters.db from component JSON sources."
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="replace an existing generated database",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    script_dir = Path(__file__).resolve().parent
    db_path = script_dir / DB_FILE

    if db_path.exists():
        if not args.force:
            print(f"Database already exists: {db_path}")
            print("Use --force to regenerate it from the tracked JSON sources.")
            return 0
        db_path.unlink()

    try:
        with sqlite3.connect(db_path) as conn:
            create_tables(conn)
            loaded = load_component_sources(conn, script_dir)

        generate_pv_performance(script_dir, db_path)
        print(f"\nDatabase generation complete: {db_path}")
        print(f"Component JSON files loaded: {loaded}")
        return 0
    except Exception as exc:
        print(f"Database generation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
