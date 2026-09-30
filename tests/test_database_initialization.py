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
            cwd=self.temporary.name, text=True, capture_output=True, timeout=30,
        )

    def test_skip_flag_preserves_component_loading_and_existing_database(self):
        result = self.run_initializer("--skip-pv")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with sqlite3.connect(self.database) as connection:
            for table in ("capacitor", "fan_cooling", "power_module", "pcb", "pv_inverter", "grid"):
                self.assertGreater(connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0], 0)
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM pv_performance_maps").fetchone()[0], 0)
        self.assertFalse(self.database.with_name("runtime_iv_curves.db").exists())
        before = self.database.read_bytes()
        result = self.run_initializer("--skip-pv")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.database.read_bytes(), before)

    def assert_runtime_curves(self, path):
        with sqlite3.connect(path) as connection:
            count = connection.execute("SELECT COUNT(*) FROM pv_performance_maps").fetchone()[0]
            self.assertEqual(count, 1116)
            voc = connection.execute(
                "SELECT Voc FROM pv_performance_maps WHERE PartNumber=? AND Irradiance=1000 AND Temperature=25",
                ("CS6U-330P",),
            ).fetchone()[0]
            self.assertAlmostEqual(voc, 45.6, delta=0.1)

    def test_default_creates_and_refreshes_runtime_curves(self):
        result = self.run_initializer()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        runtime = self.database.with_name("runtime_iv_curves.db")
        self.assert_runtime_curves(runtime)
        before = self.database.read_bytes()
        runtime.unlink()
        result = self.run_initializer()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.database.read_bytes(), before)
        self.assert_runtime_curves(runtime)

    def test_explicit_runtime_flag_and_custom_output(self):
        output = Path(self.temporary.name) / "nested" / "iv.db"
        result = self.run_initializer("--runtime-iv-curve", "--iv-database", str(output))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_runtime_curves(output)

    def test_same_database_rejected_before_force(self):
        self.database.write_bytes(b"preserve existing data")
        result = self.run_initializer("--force", "--iv-database", str(self.database))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("separate paths", result.stderr)
        self.assertEqual(self.database.read_bytes(), b"preserve existing data")

    def test_generator_failure_is_reported(self):
        result = self.run_initializer("--panel", "missing-panel.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Runtime I–V generation failed", result.stderr)
        self.assertNotIn("completed successfully", result.stdout)

    def test_incomplete_database_fails_and_explicit_force_repairs_it(self):
        result = self.run_initializer("--skip-pv")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with sqlite3.connect(self.database) as connection:
            connection.execute("DROP TABLE grid")
        result = self.run_initializer("--skip-pv")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing table: grid", result.stderr)
        result = self.run_initializer("--force", "--skip-pv")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with sqlite3.connect(self.database) as connection:
            self.assertGreater(connection.execute("SELECT COUNT(*) FROM grid").fetchone()[0], 0)


if __name__ == "__main__":
    unittest.main()
