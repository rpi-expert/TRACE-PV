#!/usr/bin/env python3
"""Exercise TRACE-PV's real argument parser without CUDA or simulation.

The production CliOptions/parser block is compiled verbatim; only its unrelated
precision/modulation types and modulation parser are stubbed.  A separate
syntax-only check covers the entire main translation unit with minimal CUDA
and OpenMP declarations.  Neither check builds GPU code, links the application,
or validates end-to-end physics.

Run from any directory with: python3 tests/output_cli_test.py
"""

from __future__ import annotations

import itertools
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]

PREAMBLE = r"""
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
constexpr int kStaticCasesPerYear5Min = 365 * 24 * 12;
enum class ComputePrecision { Float, Double };
enum class ModulationType { SVM, SPWM };
ModulationType parse_modulation(const std::string& name) {
    if (name == "svm" || name == "SVM" || name == "regular") {
        return ModulationType::SVM;
    }
    if (name == "spwm" || name == "SPWM") {
        return ModulationType::SPWM;
    }
    throw std::invalid_argument("Unknown modulation type: " + name);
}
"""

HARNESS = r"""
int main(int argc, char** argv) {
    try {
        const auto opts = parse_arguments(argc, argv);
        std::cout << "topology=" << opts.topology << '\n'
                  << "wall_time=" << opts.export_wall_time << '\n'
                  << "lifetime=" << opts.export_lifetime << '\n'
                  << "verbose=" << opts.verbose << '\n'
                  << "output_dir=" << opts.report_output_dir << '\n'
                  << "output_dir_set=" << opts.report_output_dir_set << '\n'
                  << "input_mode=" << opts.input_mode << '\n'
                  << "static_cases=" << opts.static_cases << '\n'
                  << "rounds=" << opts.num_rounds << '\n'
                  << "max_iterations=" << opts.max_iterations << '\n'
                  << "validation_output_dir=" << opts.validation_output_dir << '\n'
                  << "validation_waveform_cases=" << opts.validation_waveform_cases << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
"""


class OutputCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            raise RuntimeError("A C++17 compiler is required (set CXX if necessary)")
        source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        start = source.index("struct CliOptions {")
        end = source.index("// Topology selection removed", start)
        cls.workspace = tempfile.TemporaryDirectory(prefix="tracepv-output-cli-")
        cls.addClassCleanup(cls.workspace.cleanup)
        workdir = Path(cls.workspace.name)
        harness = workdir / "output_cli_harness.cpp"
        harness.write_text(PREAMBLE + source[start:end] + HARNESS, encoding="utf-8")
        cls.executable = workdir / "output_cli_harness"
        cls.compiler = compiler
        result = subprocess.run(
            compiler + ["-std=c++17", "-Wall", "-Wextra", str(harness), "-o", str(cls.executable)],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode:
            raise RuntimeError("Actual CLI parser did not compile:\n" + result.stdout + result.stderr)

    def run_cli(self, *args):
        return subprocess.run(
            [str(self.executable), *args],
            capture_output=True,
            text=True,
            cwd=self.workspace.name,
            timeout=10,
        )

    def parse_ok(self, *args):
        result = self.run_cli("--topology", "3l2s", *args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def assert_rejected(self, *args, contains=None):
        result = self.run_cli("--topology", "3l2s", *args)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        if contains is not None:
            self.assertIn(contains, result.stderr)

    def test_default_has_no_optional_exports_or_verbose_logging(self):
        options = self.parse_ok()
        self.assertEqual(options["wall_time"], "0")
        self.assertEqual(options["lifetime"], "0")
        self.assertEqual(options["verbose"], "0")
        self.assertEqual(options["output_dir"], "results/summary")
        self.assertEqual(options["output_dir_set"], "0")

    def test_every_flag_combination_is_independently_selectable(self):
        for wall_time, lifetime, verbose in itertools.product((False, True), repeat=3):
            flags = [
                flag
                for flag, enabled in (
                    ("--wall-time", wall_time),
                    ("--lifetime", lifetime),
                    ("--verbose", verbose),
                )
                if enabled
            ]
            with self.subTest(flags=flags):
                options = self.parse_ok(*flags)
                self.assertEqual(options["wall_time"], str(int(wall_time)))
                self.assertEqual(options["lifetime"], str(int(lifetime)))
                self.assertEqual(options["verbose"], str(int(verbose)))

    def test_flags_are_idempotent(self):
        options = self.parse_ok("--wall-time", "--wall-time", "--lifetime", "--lifetime", "--verbose", "--verbose")
        self.assertEqual([options[key] for key in ("wall_time", "lifetime", "verbose")], ["1", "1", "1"])

    def test_custom_output_directory_works_with_each_export(self):
        for flags in (("--wall-time",), ("--lifetime",), ("--wall-time", "--lifetime")):
            for output_dir in ("results/custom", "/tmp/TRACE-PV report with spaces", "相对路径/结果"):
                with self.subTest(flags=flags, output_dir=output_dir):
                    options = self.parse_ok(*flags, "--output-dir", output_dir)
                    self.assertEqual(options["output_dir"], output_dir)
                    self.assertEqual(options["output_dir_set"], "1")

    def test_output_directory_can_precede_export_flag(self):
        options = self.parse_ok("--output-dir", "reports", "--lifetime")
        self.assertEqual(options["output_dir"], "reports")
        self.assertEqual(options["lifetime"], "1")

    def test_output_directory_requires_an_export(self):
        for flags in ((), ("--verbose",)):
            with self.subTest(flags=flags):
                self.assert_rejected(*flags, "--output-dir", "reports", contains="--output-dir")

    def test_output_directory_rejects_missing_empty_or_option_value(self):
        for suffix in ((), ("",), ("--verbose",), ("--lifetime",), ("--not-an-option",)):
            with self.subTest(suffix=suffix):
                self.assert_rejected("--wall-time", "--output-dir", *suffix, contains="--output-dir")

    def test_flags_do_not_take_boolean_values(self):
        for flag in ("--wall-time", "--lifetime", "--verbose"):
            with self.subTest(flag=flag):
                self.assert_rejected(flag, "false")

    def test_unknown_option_remains_an_error(self):
        self.assert_rejected("--walltime", contains="Unknown option")

    def test_topology_is_still_required(self):
        result = self.run_cli("--wall-time", "--lifetime", "--verbose")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--topology is required", result.stderr)

    def test_help_lists_new_options_without_requiring_topology(self):
        for flag in ("--help", "-h"):
            with self.subTest(flag=flag):
                result = self.run_cli(flag)
                self.assertEqual(result.returncode, 0, result.stderr)
                for option in ("--wall-time", "--lifetime", "--verbose", "--output-dir"):
                    self.assertIn(option, result.stdout)

    def test_existing_static_and_validation_flags_can_be_combined(self):
        options = self.parse_ok(
            "--input-mode", "static", "--static-cases", "2",
            "--rounds", "1", "--max-iterations", "1",
            "--validation-output-dir", "validation results",
            "--validation-waveform-cases", "none",
            "--wall-time", "--lifetime", "--verbose",
        )
        self.assertEqual(options["input_mode"], "static")
        self.assertEqual(options["static_cases"], "2")
        self.assertEqual(options["rounds"], "1")
        self.assertEqual(options["max_iterations"], "1")
        self.assertEqual(options["validation_output_dir"], "validation results")
        self.assertEqual(options["validation_waveform_cases"], "none")

    def test_parsing_does_not_create_export_directory(self):
        output_dir = Path(self.workspace.name) / "not-created-by-parsing"
        self.parse_ok("--wall-time", "--lifetime", "--output-dir", str(output_dir))
        self.assertFalse(output_dir.exists())

    def test_main_reporting_integration_is_valid_cpp_syntax(self):
        """Check real main/reporting interfaces; do not emulate GPU execution."""
        stub_dir = Path(self.workspace.name) / "syntax-only-stubs"
        stub_dir.mkdir(exist_ok=True)
        (stub_dir / "cuda_runtime.h").write_text(
            "#pragma once\n"
            "#include <cstddef>\n"
            "enum cudaError_t { cudaSuccess = 0 };\n"
            "cudaError_t cudaSetDevice(int);\n"
            "const char* cudaGetErrorString(cudaError_t);\n",
            encoding="utf-8",
        )
        (stub_dir / "omp.h").write_text(
            "#pragma once\n"
            "void omp_set_dynamic(int);\n"
            "void omp_set_max_active_levels(int);\n"
            "int omp_get_max_active_levels();\n"
            "void omp_set_num_threads(int);\n"
            "int omp_get_thread_num();\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            self.compiler + [
                "-std=c++17", "-fsyntax-only", "-Wno-unknown-pragmas",
                "-I", str(stub_dir), "-I", str(ROOT), "-I", str(ROOT / "src"),
                "-I", str(ROOT / "component_database"), str(ROOT / "src/main.cpp"),
                str(ROOT / "src/multi_physics_simulator/thermal_simulation/igbt_loss_thermal_model.cpp"),
            ],
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
