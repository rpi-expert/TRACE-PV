# Database initialization

Run these commands from the repository root after installing `requirements.txt`:

```bash
python3 component_database/initialize_all.py
make test-mission-iv
```

The default runtime uses two separate databases:

| File | Purpose |
|---|---|
| `component_database/component_parameters.db` | Component parameters for capacitor, fan, power module, PCB, inverter and grid |
| `component_database/runtime_iv_curves.db` | Corrected per-module I–V curves selected by the model JSON |

Runtime I–V generation is enabled by default; `--runtime-iv-curve` explicitly selects the same behavior. `--skip-pv` initializes only component parameters. The former `--skip-legacy-pv` option has been removed.

The initializer validates existing component records and returns nonzero for an incomplete database. Complete component data is left unchanged, while corrected curves for the selected panel are regenerated on each run. Other panels in the runtime database are preserved. Generation failures return nonzero.

`--database PATH` selects the component database. Runtime curves default to `runtime_iv_curves.db` in the same directory; use `--iv-database PATH` to override this. The two database paths must differ. Relative paths are resolved from the current working directory; default panel and component paths are resolved from the repository. For example:

```bash
python3 component_database/initialize_all.py --runtime-iv-curve \
  --database /tmp/tracepv/components.db \
  --iv-database /tmp/tracepv/curves.db \
  --panel component_database/pv_panel/CS6U-330P.json
```

Update the simulation model's database paths when using custom outputs.

After editing component JSON, back up the existing database before explicitly rebuilding it:

```bash
python3 component_database/initialize_all.py --force
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

Do not use `offline_trainning/offline_data_generator.py` to supply the runtime model: the legacy module-voltage calculation omits the series cell count. The independent legacy `verify_database.py` checks the old all-panel database layout and is not a verifier for the separate selected-panel runtime database. Use `make test-mission-iv` for the default runtime model.

Read the [root setup guide](../README.md) for field inputs, thermal lookup assets and GPU smoke tests.
