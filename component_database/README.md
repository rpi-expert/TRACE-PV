# Component and I–V databases

Use the [initialization guide](INITIALIZATION_GUIDE.md) for the current runtime workflow. From the repository root:

```bash
python3 component_database/initialize_all.py
make test-mission-iv
```

- `component_parameters.db` stores component parameters loaded from `capacitor/`, `fan_cooling/`, `power_module/`, `pcb/`, `pv_inverter/` and `grid/` JSON files.
- `runtime_iv_curves.db` stores the corrected per-module I–V grid for the configured panel. Runtime C++ lookup is implemented in `src/simulation_preparation/iv_database.h`; array scaling comes from the simulation-model JSON.
- `initialize_all.py` creates/verifies component parameters and generates corrected runtime curves by default (`--runtime-iv-curve`). Use `--skip-pv` for component-only initialization. Existing-database validation, `--database` and `--force` remain supported.
- `init_database.py` supplies schemas and JSON loaders; `component_database.cpp/.h` supplies the runtime component interface.
- The old offline PV generator and all-panel verifier remain legacy tools. Their database layout does not validate the corrected separate runtime I–V database. The old generator omits the module's series cell count and must not replace the runtime curve builder.

To add a component, add its JSON to the matching folder, back up the existing database, and rebuild component tables with `--force`. To change a PV panel, generate its runtime curves and update the model's panel name, database path, `modules_per_string` and `parallel_strings`.

Python's `sqlite3` handles database preparation; runtime I–V generation additionally needs NumPy/SciPy. The simulator links SQLite through the Makefile. See the [root README](../README.md) for supported Python/CUDA environments and tests.
