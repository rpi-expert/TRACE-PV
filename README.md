# TRACE-PV

TRACE-PV is a GPU-accelerated multi-physics simulator for photovoltaic inverter reliability assessment. It combines electrical, thermal, and environmental simulation with component degradation models driven by mission profiles and a component database.

## Runtime Setup

### Prerequisites

- **Linux** with a **NVIDIA GPU** and CUDA toolkit (`/usr/local/cuda` by default)
- **Python 3.8+** and `python3-venv`
- **g++**, **nvcc**, `pkg-config`, and the SQLite development package (`libsqlite3-dev` on Debian/Ubuntu)

### 1. Activate the environment

From the project root:

```bash
source setup_env.sh
```

This script:

- Adds a project-local `sqlite3/lib` to `LD_LIBRARY_PATH` only when that optional directory exists
- Activates the project virtual environment at `venv/` when present
- Prepends CUDA tools to `PATH`

### 2. Set up the Python virtual environment

If `venv/` does not exist or is broken, create and populate it:

```bash
python3 -m venv venv
source venv/bin/activate
python3 -m pip install --upgrade pip
python3 -m pip install -r requirements.txt
```

The NSRDB download wrapper (`simulator_inputs/scripts/run_nrsdb.sh`) uses this project-level `venv/`.

### 3. Initialize the component database

From the project root, run the unified initializer:

```bash
python3 component_database/initialize_all.py
```

The command is safe to run again: it skips component and PV data that is
already complete. To rebuild the database after changing component or PV JSON
files, use:

```bash
python3 component_database/initialize_all.py --force
```

Verify the resulting database independently with:

```bash
cd component_database
python3 verify_database.py
```

The initializer creates `component_database/component_parameters.db`, loads all
component parameters, and invokes the checked-in Python PV generator to build
the IV-curve lookup data from `pv_panel/*.json`. It does not require a compiled
database-generation helper, and it returns a non-zero exit code if component
loading, PV generation, or verification fails.

To regenerate PV performance data only:

```bash
cd component_database
python3 offline_trainning/offline_data_generator.py component_parameters.db pv_panel
```

See `component_database/INITIALIZATION_GUIDE.md` for details.

### 4. Build the simulator

```bash
make
```

The Makefile honors `NVCC` from the command line or environment. If `NVCC` is
unset, it searches `PATH` and then falls back to `CUDA_PATH/bin/nvcc`;
`CUDA_HOME` is also accepted as the default CUDA path. For example:

```bash
export NVCC=/opt/cuda/bin/nvcc
make

# Equivalent one-command override
make NVCC=/opt/cuda/bin/nvcc
```

The Makefile also auto-detects the GPU compute capability (for example,
`sm_90` for H200) and uses the system SQLite development library. A
project-local SQLite installation under `sqlite3/` is still detected when
supplied separately.

### 5. Prepare mission profile inputs (optional)

Mission profiles are loaded automatically from fixed paths (see [Running the Code](#running-the-code)). To download environmental data from NRSDB:

```bash
export NSRDB_API_KEY="your-api-key"
./simulator_inputs/scripts/run_nrsdb.sh
```

For the React mission-profile downloader, set `REACT_APP_NSRDB_API_KEY` before starting or building the WebUI. See `.env.example`; local `.env` files are excluded from version control.

---

## Running the Code

### Simulator binary

```bash
./bin/trace_pv --topology <TOPOLOGY> [OPTIONS]
```

#### Required

| Option | Short | Description |
|--------|-------|-------------|
| `--topology` | `-t` | Inverter topology: `2l2s`, `2l1s`, `3l2s`, or `3l1s` |

| Topology | Meaning |
|----------|---------|
| `2l2s` | Two-level, two-stage |
| `2l1s` | Two-level, single-stage |
| `3l2s` | Three-level, two-stage |
| `3l1s` | Three-level, single-stage |

#### Optional

| Option | Short | Default | Description |
|--------|-------|---------|-------------|
| `--rounds` | `-r` | `1` | Number of partitions (rounds) within each mission-profile iteration |
| `--max-iterations` | | `0` | Maximum mission-profile repeats; `0` runs until degradation reaches 1.0 |
| `--modulation` | `-m` | `svm` | Modulation strategy: `svm` or `spwm` |
| `--ngpus` | `-g` | all available | Number of GPUs to use, or `all` |
| `--model` | `-M` | `simulator_inputs/simulation_model/example_simulation_model.json` | Simulation model JSON with component part numbers |
| `--input-mode` | | `mission` | Input mode: `mission` or `static` |
| `--mission-csv` / `--csv` | `-c` | unset | Combined mission profile CSV |
| `--environmental-csv` | | fixed default | Environmental CSV for split mission profile mode |
| `--operating-csv` | | fixed default | Operating CSV for split mission profile mode |
| `--static-temp` | | `95` | Static ambient temperature in Celsius |
| `--static-rh` | | `95` | Static relative humidity in percent |
| `--static-voltage` | | `500` | Static AC voltage RMS line-to-line in volts |
| `--static-power` | | `300` | Static AC power in watts for the thermal model |
| `--static-irradiance` | | `1000` | Static solar irradiance in W/m² |
| `--static-cases` | | `105120` | Number of repeated 5-minute static cases (one year) |
| `--validation-output-dir` | | disabled | Export a model-validation intermediate summary and selected A2S matrices |
| `--validation-waveform-cases` | | `0` | `none`, `all`, or comma-separated zero-based case indices |
| `--wall-time` | | disabled | Export `wall_time.json` with elapsed wall-clock seconds |
| `--lifetime` | | disabled | Export `lifetime.csv` and `lifetime.json` with component damage, projected lifetime and failure status |
| `--output-dir` | | `results/summary` | Destination for selected wall-time/lifetime reports; requires one or both export flags |
| `--verbose` | | disabled | Restore detailed diagnostic/profiling console output; warnings and progress remain visible by default |

#### Limit the number of iterations

Run from the repository root after rebuilding the binary on a Linux/CUDA host:

```bash
make -j4

# Run one mission-profile iteration
./bin/trace_pv --topology 3l2s --max-iterations 1

# One iteration with detailed diagnostics and both optional report exports
./bin/trace_pv --topology 3l2s --max-iterations 1 \
  --verbose --wall-time --lifetime --output-dir results/one_iteration
```

The option is `--max-iterations` (plural), not `--max-iteration`. An iteration is
one pass through the loaded mission profile, not one case or necessarily one
year. `--rounds` partitions that pass; it does not set the number of profile
repeats. `--max-iterations 0` (the default) has no iteration limit and runs until
degradation reaches 1.0. A positive limit can still stop earlier at that
degradation threshold.

#### Select wall-time, lifetime and verbose output

Other output combinations:

```bash
# Wall-time report only (among the new optional reports)
./bin/trace_pv --topology 3l2s --wall-time

# Component lifetime projections, accumulated damage and detected-failure status
./bin/trace_pv --topology 3l2s --lifetime

# Detailed console diagnostics
./bin/trace_pv --topology 3l2s --verbose

# Combine all three and choose the report destination
./bin/trace_pv --topology 3l2s \
  --wall-time --lifetime --verbose --output-dir results/my_run

# Small execution/export smoke test, NOT a validated lifetime study
./bin/trace_pv --topology 3l2s --input-mode static \
  --static-cases 2 --max-iterations 1 \
  --wall-time --lifetime --output-dir results/output_smoke
```

`--wall-time`, `--lifetime` and `--verbose` are presence-only flags: omit a flag
to disable that optional output; do not pass `on`, `off`, `true` or `false`.
Existing mission, static, GPU and validation options can be combined with them.

By default the console shows component configuration, progress, final accumulated
damage and wall time. `--verbose` adds detailed model and profiling diagnostics.
The WebUI explicitly requests verbose output to preserve its diagnostic cards.
New report exports are opt-in; `--output-dir` does **not** relocate or suppress
the existing stressor/thermal CSVs, timing logs or `--validation-output-dir`
exports. Requested summary files with the same names are overwritten; unselected
old report files are left untouched, so use a new output directory for each run.

Lifetime is a **constant-average-damage projection**: represented exposure hours
divided by accumulated dimensionless damage, with 8,760 hours per year. Exposure
counts accepted five-minute cases, including reused rounds but excluding
thermal-only refreshes. It does not assume one profile iteration equals a year,
does not fill missing calendar intervals, and is not a substitute for a completed
degradation-feedback simulation. Detected failure boundaries and right-censored
runs are identified separately; zero/invalid damage does not produce a fabricated
finite lifetime. See [output report definitions](docs/OUTPUT_REPORTS.md).

Portable CLI, logging and report tests (no CUDA required):

```bash
make test-reporting
```

#### Input modes

Mission profile mode is the default. If no CSV path is supplied, mission profiles are read from:

- `simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv`
- `simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv`

You can also pass one combined CSV with:

```bash
./bin/trace_pv --topology 3l2s --mission-csv simulator_inputs/mission_profile/year_long_mission_profile_2024.csv
```

Combined mission CSV columns:

```text
time,ambient_temperature,rh,GHI,ac_voltage[,ac_power]
```

Static mode creates repeated cases using one set of values. Defaults match the requested stress case: `temp=95`, `rh=95`, `voltage=500`, `power=300`.

```bash
./bin/trace_pv --topology 3l2s --input-mode static \
  --static-temp 95 --static-rh 95 --static-voltage 500 --static-power 300
```

The simulation model JSON specifies component part numbers (capacitor, power module, fan, PCB, PV panel, etc.) that must exist in `component_database/`. See `simulator_inputs/simulation_model/README.md`.

#### Examples

```bash
# Minimal run
./bin/trace_pv --topology 3l2s

# Custom model, modulation, and GPU count
./bin/trace_pv --topology 2l2s --modulation spwm --ngpus 1 \
  --model simulator_inputs/simulation_model/example_simulation_model.json

# Run a provided combined mission profile CSV
./bin/trace_pv --topology 3l2s \
  --mission-csv simulator_inputs/mission_profile/year_long_mission_profile_2024.csv

# Run static stress conditions for all cases
./bin/trace_pv --topology 3l2s --input-mode static \
  --static-temp 95 --static-rh 95 --static-voltage 500 --static-power 300

# Multiple rounds per iteration
./bin/trace_pv --topology 3l1s --rounds 6 --ngpus all

# One-case model-validation run with an A2S core matrix
./bin/trace_pv --topology 3l2s --input-mode static \
  --static-cases 1 --max-iterations 1 \
  --validation-output-dir results/model_validation \
  --validation-waveform-cases 0
```

#### Model-validation intermediate values

When `--validation-output-dir` is supplied, the simulator writes one scalar
summary per round and a three-column A2S matrix for each selected case. The
summary exposes the Average Model steady-state dq values, processed capacitor
RMS current, capacitor and inverter losses, surface/hotspot/junction
temperatures, and ambient/internal temperature and RH. Predicted internal
conditions are kept separate from the values actually used downstream when a
mission profile supplies an internal-temperature override.

See [Model-validation intermediate export](docs/MODEL_VALIDATION.md) for the
file layout, exact columns, and value semantics. Avoid the `all` waveform
selector on long mission profiles unless the large output volume is intended.

#### Batch runner

To run all configured topology/modulation combinations and save timestamped results:

```bash
./run_all_configs.sh [ROUNDS] [NGPUS] [MODEL_FILE]
```

| Argument | Default | Description |
|----------|---------|-------------|
| `ROUNDS` | `6` | Rounds per mission-profile iteration |
| `NGPUS` | `all` | GPU count or `all` |
| `MODEL_FILE` | `simulator_inputs/simulation_model/example_simulation_model.json` | Simulation model JSON |

Output is written to `results/results_<timestamp>/`.

#### Make shortcut

```bash
make run TOPOLOGY=2l2s ROUNDS=1 MOD=svm NGPUS=1
make run TOPOLOGY=3l2s CSV=simulator_inputs/mission_profile/year_long_mission_profile_2024.csv
make run TOPOLOGY=3l2s MODE=static
```

---

## Loss / Thermal Source Layout

In `src/multi_physics_simulator/thermal_simulation/`:

- `capacitor_loss_thermal_model.cpp` and `igbt_loss_thermal_model.cpp` implement the detailed models.
- `capacitor_reference_gpu.cu` and `igbt_reference_gpu.cu` contain their GPU implementations; detailed CPU paths remain available.
- `simplified_loss_thermal.h` contains only the live host-only approximations used by `fast`, invalid-reference fallback, and capacitor thermal-only refresh. It is **not** the detailed model.

The unused legacy CUDA kernels and V/I-only IGBT loss API have been removed, along with the old `loss_model.cu/.h` and `thermal_model.cu/.h` files and their build entries. This is a dead-code cleanup, not a change to simulation physics: the existing simplified fallback remains reachable, even in `reference` mode when a detailed result is invalid.

Run `make test-loss-thermal-cleanup` for host-only compatibility tests. Rebuild with `make -j4` on the Linux/CUDA host before using the updated simulator; an existing binary is not updated by deleting source files.

## Development Progress

### Database

- [x] Mission Profile (Environmental) from NRSDB
- [x] Mission Profile (Operating)
- [x] PV Inverter Database
- [x] PV Panel Database
- [ ] Control Database

### Simulator

- [x] Electrical Simulation
- [x] Loss Simulation
- [x] Thermal Simulation
- [x] Environmental Simulation

### Model Validation

- [x] Electrical Simulation
- [x] Loss Simulation
- [x] Thermal Simulation
- [x] Environmental Simulation

---

## Project Layout

```
TRACE-PV/
├── component_database/       # Component JSON files and SQLite database
├── deploy/                   # Example WebUI service configuration
├── plots/scripts/            # Result plotting source
├── simulator_inputs/         # Mission profiles and simulation model JSON
├── src/                      # C++/CUDA simulator source
├── webui/                    # React WebUI source and Python API server
├── requirements.txt          # Python runtime dependencies
├── setup_env.sh              # Runtime environment activation
├── Makefile                  # Build system
└── run_all_configs.sh        # Batch simulation runner
```

`bin/`, `results/`, `logs/`, WebUI build output, package caches, and virtual environments are generated locally and intentionally excluded from version control.

## Further Reading

- `component_database/INITIALIZATION_GUIDE.md` — Database setup
- `simulator_inputs/README.md` — Input file formats
- `src/README.md` — Simulator architecture and batch processing details
