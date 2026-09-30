#!/usr/bin/env python3
"""Initialize and verify the complete TRACE-PV component database.

This entry point uses the current JSON loaders from ``init_database.py`` and
the corrected runtime I–V generator by default. Component parameters and
runtime curves are stored in separate SQLite databases.
"""

import argparse
import json
import sqlite3
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple

import init_database


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_DB_PATH = SCRIPT_DIR / init_database.DB_FILE
PV_PANEL_DIR = SCRIPT_DIR / "pv_panel"
PV_GENERATOR = SCRIPT_DIR.parent / "tools" / "build_runtime_iv_database.py"

COMPONENT_LOADERS = {
    "capacitor": init_database.load_capacitor,
    "fan_cooling": init_database.load_fan_cooling,
    "power_module": init_database.load_power_module,
    "pcb": init_database.load_pcb,
    "pv_inverter": init_database.load_pv_inverter,
    "grid": init_database.load_grid,
}


def resolve_database_path(value: str) -> Path:
    """Resolve a database path supplied on the command line."""
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = Path.cwd() / path
    return path.resolve()


def expected_component_parts(component_type: str, json_dir: Path) -> Set[str]:
    """Read the expected part numbers for one component directory."""
    expected = set()
    for json_path in sorted(json_dir.glob("*.json")):
        with json_path.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
        part_number = data.get("part_number")
        if component_type == "grid" and not part_number:
            part_number = json_path.stem
        if not part_number:
            raise ValueError(f"No part_number found in {json_path}")
        expected.add(str(part_number))
    return expected


def component_database_status(db_path: Path) -> Tuple[bool, List[str]]:
    """Check that every component JSON file is represented in the database."""
    if not db_path.exists():
        return False, [f"Database does not exist: {db_path}"]

    problems = []
    try:
        with sqlite3.connect(str(db_path)) as connection:
            cursor = connection.cursor()
            cursor.execute("SELECT name FROM sqlite_master WHERE type='table'")
            tables = {row[0] for row in cursor.fetchall()}

            for component_type, folder in init_database.COMPONENT_FOLDERS.items():
                json_dir = SCRIPT_DIR / folder
                if component_type not in tables:
                    problems.append(f"Missing table: {component_type}")
                    continue
                if not json_dir.is_dir():
                    problems.append(f"Missing component directory: {json_dir}")
                    continue

                try:
                    expected = expected_component_parts(component_type, json_dir)
                except (OSError, ValueError, json.JSONDecodeError) as exc:
                    problems.append(str(exc))
                    continue

                cursor.execute(f"SELECT part_number FROM {component_type}")
                loaded = {str(row[0]) for row in cursor.fetchall()}
                for part_number in sorted(expected - loaded):
                    problems.append(
                        f"Missing {component_type} component: {part_number}"
                    )
    except sqlite3.Error as exc:
        problems.append(f"Could not inspect {db_path}: {exc}")

    return not problems, problems


def initialize_components(db_path: Path) -> bool:
    """Create the schema and load all component JSON files."""
    db_path.parent.mkdir(parents=True, exist_ok=True)
    errors = []
    loaded_count = 0

    with sqlite3.connect(str(db_path)) as connection:
        init_database.create_tables(connection)

        for component_type, folder in init_database.COMPONENT_FOLDERS.items():
            json_dir = SCRIPT_DIR / folder
            loader = COMPONENT_LOADERS[component_type]
            json_files = sorted(json_dir.glob("*.json"))
            print(f"Loading {component_type}: {len(json_files)} file(s)")

            if not json_files:
                errors.append(f"No JSON files found in {json_dir}")
                continue

            for json_path in json_files:
                try:
                    part_number = loader(connection, json_path)
                    print(f"  Loaded {part_number} from {json_path.name}")
                    loaded_count += 1
                except Exception as exc:  # Report the source file that failed.
                    errors.append(f"{json_path}: {exc}")

    if errors:
        print("Component initialization failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return False

    print(f"Loaded {loaded_count} component definition(s).")
    return True


def generate_pv_data(db_path: Path, panel_path: Path) -> bool:
    """Build corrected runtime curves; the generator checks STC voltage/power."""
    db_path.parent.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable, str(PV_GENERATOR),
        "--panel", str(panel_path), "--output", str(db_path),
    ]
    print(f"Generating runtime I–V curves: {db_path}", flush=True)
    result = subprocess.run(command, cwd=str(SCRIPT_DIR), check=False)
    if result.returncode != 0:
        print(
            f"Runtime I–V generation failed with exit code {result.returncode}.",
            file=sys.stderr,
        )
        return False
    return True


def database_counts(db_path: Path) -> Dict[str, int]:
    """Collect final row counts for the initialization summary."""
    counts = {}
    with sqlite3.connect(str(db_path)) as connection:
        cursor = connection.cursor()
        for table in COMPONENT_LOADERS:
            cursor.execute(f"SELECT COUNT(*) FROM {table}")
            counts[table] = int(cursor.fetchone()[0])
    return counts


def print_problems(title: str, problems: List[str]) -> None:
    print(title, file=sys.stderr)
    for problem in problems:
        print(f"  - {problem}", file=sys.stderr)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Initialize and verify the TRACE-PV component database."
    )
    parser.add_argument(
        "--database",
        default=str(DEFAULT_DB_PATH),
        help=f"SQLite database path (default: {DEFAULT_DB_PATH})",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Remove and rebuild an existing database.",
    )
    pv_options = parser.add_mutually_exclusive_group()
    pv_options.add_argument(
        "--runtime-iv-curve", dest="runtime_iv_curve", action="store_true",
        help="Generate corrected runtime I–V curves (default).",
    )
    pv_options.add_argument(
        "--skip-pv", dest="runtime_iv_curve", action="store_false",
        help="Initialize only component parameters; skip runtime I–V generation.",
    )
    parser.set_defaults(runtime_iv_curve=True)
    parser.add_argument(
        "--panel", default=str(PV_PANEL_DIR / "CS6U-330P.json"),
        help="Panel JSON for runtime curves (default: CS6U-330P).",
    )
    parser.add_argument(
        "--iv-database",
        help="Runtime curve database (default: runtime_iv_curves.db beside --database).",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    db_path = resolve_database_path(args.database)

    iv_path = (resolve_database_path(args.iv_database) if args.iv_database
               else db_path.with_name("runtime_iv_curves.db"))
    if args.runtime_iv_curve and iv_path == db_path:
        print("Component and runtime I–V databases must use separate paths.", file=sys.stderr)
        return 1

    print(f"TRACE-PV database: {db_path}")
    if args.force and db_path.exists():
        print("Removing the existing database because --force was supplied.")
        db_path.unlink()

    if not db_path.exists():
        if not initialize_components(db_path):
            return 1
    else:
        components_ok, component_problems = component_database_status(db_path)
        if not components_ok:
            print_problems("The existing component database is incomplete:", component_problems)
            print("Run again with --force to rebuild it.", file=sys.stderr)
            return 1
        print("Component data is already complete; skipping component initialization.")

    components_ok, component_problems = component_database_status(db_path)
    if not components_ok:
        print_problems("Component verification failed:", component_problems)
        return 1

    if args.runtime_iv_curve:
        if not generate_pv_data(iv_path, resolve_database_path(args.panel)):
            return 1
    else:
        print("Runtime I–V generation skipped by request.")

    counts = database_counts(db_path)
    print("Database initialization and verification completed successfully.")
    print(
        "Component rows: "
        + ", ".join(f"{name}={counts[name]}" for name in COMPONENT_LOADERS)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
