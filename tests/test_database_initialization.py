"""Integration checks for preserving preview's initializer with the runtime-IV workflow."""
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INITIALIZER = ROOT / "component_database" / "initialize_all.py"


class DatabaseInitializationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.database = Path(self.temporary.name) / "components.db"

    def run_initializer(self, *flags):
        return subprocess.run(
            [sys.executable, str(INITIALIZER), "--database", str(self.database), *flags],
            cwd=ROOT, text=True, capture_output=True, timeout=30,
        )

    def test_new_skip_flag_preserves_component_loading_and_existing_database(self):
        result = self.run_initializer("--skip-legacy-pv")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with sqlite3.connect(self.database) as connection:
            for table in ("capacitor", "fan_cooling", "power_module", "pcb", "pv_inverter", "grid"):
                self.assertGreater(connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0], 0)
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM pv_performance_maps").fetchone()[0], 0)
        before = self.database.read_bytes()
        result = self.run_initializer("--skip-pv")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.database.read_bytes(), before)

    def test_incomplete_database_fails_and_explicit_force_repairs_it(self):
        result = self.run_initializer("--skip-pv")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with sqlite3.connect(self.database) as connection:
            connection.execute("DROP TABLE grid")
        result = self.run_initializer("--skip-legacy-pv")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing table: grid", result.stderr)
        result = self.run_initializer("--force", "--skip-legacy-pv")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with sqlite3.connect(self.database) as connection:
            self.assertGreater(connection.execute("SELECT COUNT(*) FROM grid").fetchone()[0], 0)


if __name__ == "__main__":
    unittest.main()
