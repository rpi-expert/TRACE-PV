# TRACE-PV

TRACE-PV is a GPU-accelerated multi-physics simulator for photovoltaic inverter reliability assessment. It combines electrical, thermal, and environmental simulation with component degradation models driven by mission profiles and a component database.

## Runtime Setup

### Prerequisites

- **Linux** with a **NVIDIA GPU** and CUDA toolkit (`/usr/local/cuda` by default)
- **Python 3.8+** (standard library only for database scripts; `pandas` and `requests` for NSRDB download)
- **g++** and **nvcc** for building the simulator

### 1. Activate the environment

From the project root:

```bash
source setup_env.sh
```

This script:

- Adds `sqlite3/lib` to `LD_LIBRARY_PATH`
- Activates the project virtual environment at `venv/`
- Prepends CUDA tools to `PATH`

### 2. Set up the Python virtual environment

If `venv/` does not exist or is broken, create and populate it:

```bash
python3 -m venv venv
source venv/bin/activate
python3 -m pip install --upgrade pip
python3 -m pip install pandas requests numpy matplotlib
```

The NSRDB download wrapper (`simulator_inputs/scripts/run_nrsdb.sh`) uses this project-level `venv/`.

### 3. Initialize the component database

First-time setup (or after adding new component JSON files):

```bash
cd component_database
python3 initialize_all.py
python3 verify_database.py
```

This creates `component_database/component_parameters.db`, loads component parameters (capacitor, fan, power module, PCB), and generates PV panel IV-curve lookup data from `pv_panel/*.json`.

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

The Makefile auto-detects the GPU compute capability (e.g. `sm_90` for H200) and links against the bundled SQLite library in `sqlite3/`.

If linking fails due to a GLIBC mismatch with the pre-built `sqlite3/lib/libsqlite3.so`, rebuild SQLite locally:

```bash
# Example: rebuild from the SQLite amalgamation into sqlite3/lib/
gcc -shared -fPIC -O2 sqlite3.c -o sqlite3/lib/libsqlite3.so -ldl -lpthread -lm
make clean && make
```

### 5. Prepare mission profile inputs (optional)

Mission profiles are loaded automatically from fixed paths (see [Running the Code](#running-the-code)). To download environmental data from NRSDB:

```bash
./simulator_inputs/scripts/run_nrsdb.sh
```

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
| `--rounds` | `-r` | `1` | Number of rounds to process per mission-profile iteration |
| `--modulation` | `-m` | `svm` | Modulation strategy: `svm` or `spwm` |
| `--ngpus` | `-g` | all available | Number of GPUs to use, or `all` |
| `--model` | `-M` | `simulator_inputs/simulation_model/example_simulation_model.json` | Simulation model JSON with component part numbers |

#### Automatically loaded inputs

The simulator does **not** take a CSV path on the command line. Mission profiles are read from:

- `simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv`
- `simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv`

The simulation model JSON specifies component part numbers (capacitor, power module, fan, PCB, PV panel, etc.) that must exist in `component_database/`. See `simulator_inputs/simulation_model/README.md`.

#### Examples

```bash
# Minimal run
./bin/trace_pv --topology 3l2s

# Custom model, modulation, and GPU count
./bin/trace_pv --topology 2l2s --modulation spwm --ngpus 1 \
  --model simulator_inputs/simulation_model/example_simulation_model.json

# Multiple rounds per iteration
./bin/trace_pv --topology 3l1s --rounds 6 --ngpus all
```

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
```

> **Note:** `make run` uses the legacy CSV-based interface and may differ from the current `trace_pv` CLI. Prefer `./bin/trace_pv` directly.

---

## Development Progress

### Database

- [x] Mission Profile (Environmental) from NRSDB
- [x] Mission Profile (Operating)
- [x] PV Inverter Database
- [x] PV Panel Database
- [ ] Control Database

### Simulator

- [x] Electrical Simulation
- [ ] Loss Simulation
- [ ] Thermal Simulation
- [ ] Environmental Simulation

### Model Validation

- [ ] Electrical Simulation
- [ ] Loss Simulation
- [ ] Thermal Simulation
- [ ] Environmental Simulation

---

## Project Layout

```
TRACE-PV/
├── bin/trace_pv              # Simulator executable (built by make)
├── component_database/       # Component JSON files and SQLite database
├── simulator_inputs/         # Mission profiles and simulation model JSON
├── src/                      # C++/CUDA simulator source
├── results/                  # Simulation output
├── setup_env.sh              # Runtime environment activation
├── Makefile                  # Build system
└── run_all_configs.sh        # Batch simulation runner
```

## Further Reading

- `component_database/INITIALIZATION_GUIDE.md` — Database setup
- `simulator_inputs/README.md` — Input file formats
- `src/README.md` — Simulator architecture and batch processing details
