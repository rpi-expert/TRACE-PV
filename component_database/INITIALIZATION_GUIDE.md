# Database initialization

Run these commands from the repository root after installing `requirements.txt`:

```bash
python3 component_database/initialize_all.py --skip-legacy-pv
python3 tools/build_runtime_iv_database.py
make test-mission-iv
```

The default runtime uses two separate databases:

| File | Purpose |
|---|---|
| `component_database/component_parameters.db` | Component parameters for capacitor, fan, power module, PCB, inverter and grid |
| `component_database/runtime_iv_curves.db` | Corrected per-module I–V curves selected by the model JSON |

`--skip-legacy-pv` is an alias for preview's existing `--skip-pv`. Both skip the legacy PV generator while creating and verifying all component tables. The initializer still supports `--database PATH`, validates existing component records, and returns nonzero for an incomplete database. It does not silently replace existing data.

After editing component JSON, back up the existing database before explicitly rebuilding it:

```bash
python3 component_database/initialize_all.py --force --skip-legacy-pv
```

An isolated component-database check is also supported:

```bash
python3 component_database/initialize_all.py --database /tmp/tracepv-components.db --skip-pv
```

The runtime I–V generator uses the component's cell count and single-diode parameters, checks STC voltage/power, and writes the selected panel's curves to a separate database. Its default CS6U-330P grid spans GHI 0.01–1600 W/m² and temperature −45–105 °C. The simulation model selects the database and separately specifies modules per string and parallel strings.

```bash
python3 tools/build_runtime_iv_database.py \
  --panel component_database/pv_panel/CS6U-330P.json \
  --output component_database/runtime_iv_curves.db
```

Do not use `offline_trainning/offline_data_generator.py` to supply the runtime model: the legacy module-voltage calculation omits the series cell count. Legacy initializer behavior without a skip flag and the independent `verify_database.py` remain available for compatibility; the latter checks the old all-panel database layout and is not a verifier for the separate selected-panel runtime database. Use `make test-mission-iv` for the default runtime model.

Read the [root setup guide](../README.md) for field inputs, thermal lookup assets and GPU smoke tests.
