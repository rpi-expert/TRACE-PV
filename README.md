# TRACE-PV

TRACE-PV is a C++/CUDA simulator for photovoltaic inverter electrical stress, loss, temperature, humidity and component degradation. Run commands below from the **repository root** in a Bash shell. Start with the two-case smoke test before running a full mission profile.

## 1. Check the Linux/CUDA host

You need an NVIDIA GPU, a CUDA toolkit that supports that GPU, a compatible C++17 compiler, GNU Make, SQLite development files, and Python 3.10 or newer. Python is used for preparing inputs/databases and the optional WebUI; the simulator itself is a compiled binary.

```bash
git clone https://github.com/rpi-expert/TRACE-PV.git
cd TRACE-PV
nvidia-smi
nvcc --version
g++ --version
python3 --version
```

`nvidia-smi` reports the driver's supported CUDA version, which may differ from the installed toolkit shown by `nvcc`. V100 requires a toolkit that still targets `sm_70`, such as CUDA 12.8. Install toolkit packages only on a managed GPU host; do not replace its NVIDIA driver.

On Ubuntu/Debian, install missing host dependencies:

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libsqlite3-dev python3-venv
```

Omit `sudo` when already root. If CUDA is not under `/usr/local/cuda`, set `CUDA_PATH` to the installed toolkit directory before sourcing the environment script.

## 2. Create the Python environment

```bash
python3 -m venv venv
source venv/bin/activate
python3 -m pip install --upgrade pip
python3 -m pip install -r requirements.txt
source setup_env.sh
```

Do not copy a macOS virtual environment or compiled binary to Linux. Recreate the environment and build on the target host. `setup_env.sh` activates an existing `venv`, adds CUDA to `PATH`, and adds the optional project SQLite library to `LD_LIBRARY_PATH`; it does not install dependencies.

## 3. Prepare both databases

The default model needs a **component database** and a separate **runtime I–V curve database**:

```bash
python3 component_database/initialize_all.py --skip-legacy-pv
python3 tools/build_runtime_iv_database.py
```

The first command creates `component_database/component_parameters.db` from component JSON files, including inverter and grid tables. It leaves an existing database unchanged. To rebuild an existing database, first back it up, then use `--force --skip-legacy-pv`.

The second command generates `component_database/runtime_iv_curves.db` for CS6U-330P, with an STC sanity check against the panel specification. It uses the 72-cell single-diode model and covers 0.01–1600 W/m² and −45–105 °C. Do not substitute the legacy offline I–V generator: its module thermal-voltage calculation omits the cell count.

The default model JSON explicitly selects this I–V database and an **18s6p** array. For another panel/array, update `pv_panel.part_number`, `iv_database`, `modules_per_string`, and `parallel_strings`, and generate matching curves using the tool's `--panel`/`--output` options. Invalid curves or queries outside the database grid fail explicitly.

### Thermal lookup data

Detailed thermal simulation additionally needs the project data files under:

- `IGBT/MATLAB_code/parameters/` (all CSV lookup tables)
- `Capacitor/cpp_standalone/data/esr_data.csv`

These research assets are not present in the current GitHub source checkout. Obtain them from the project data package and preserve these relative paths before running the GPU smoke test. The server deployment includes them. Without them, a successful build can still fall back to simplified thermal models; it is not a complete detailed-model deployment.

## 4. Build and test

```bash
make -j2
./bin/trace_pv --help
make test-mission-iv
make test-reporting
make test-loss-thermal-cleanup
```

The Makefile detects GPU compute capability and prefers bundled SQLite if present, otherwise system SQLite. To set a nonstandard toolkit or explicit GPU target:

```bash
make -j2 CUDA_PATH=/usr/local/cuda-12.8 GPU_SM=70
```

`test-mission-iv` checks the default panel configuration, cleaning rules and curve interpolation. It also checks the full split mission profile when the environmental CSV is available. Tests and generated databases should be refreshed after changing the model configuration.

### Two-case GPU smoke test

This committed fixture has no AC-power column, so it exercises the electrical-power prepass, I–V lookup, environmental model, and downstream simulation:

```bash
./bin/trace_pv --topology 3l2s \
  --mission-csv tests/fixtures/deployment_mission.csv \
  --max-iterations 1 --ngpus 1 --batch-size-limit 1 --pipeline off \
  --verbose --wall-time --lifetime --output-dir results/deployment_smoke \
  --validation-output-dir results/deployment_smoke/validation \
  --validation-waveform-cases none
```

Check the exit status, console warnings and generated reports. Expected progress includes `TRACEPV_IV`, `TRACEPV_POWER_PREPASS` and mission-round progress. A completed smoke test confirms execution; it does not establish field accuracy or annual lifetime validity.

## 5. Prepare and run mission data

The default split files are:

| File | Columns |
|---|---|
| `simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv` | `time,ambient_temperature,rh,GHI` |
| `simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv` | `time,ac_voltage` |

Supply your field/environmental CSVs at these paths or pass `--environmental-csv` and `--operating-csv`. A fresh GitHub clone may not contain the local environmental dataset. The two-case fixture above works without it. The optional NSRDB download workflow is under `simulator_inputs/scripts/`; it requires separate API credentials and does not create field operating measurements.

Split inputs are joined by timestamp and retain environmental-file order; provide chronological, unique timestamps. Units are °C, RH percent (0–100), GHI W/m², and AC RMS line-to-line volts. Input cleaning rejects exact −1 °C sentinels, RH ≤1% or >100%, non-finite fields, nonpositive GHI/voltage, and unmatched records. Other subzero temperatures are allowed. Raw CSVs are not rewritten.

Alternatively, use a combined CSV:

```text
time,ambient_temperature,rh,GHI,ac_voltage[,ac_power[,internal_temperature]]
```

AC power is **not required**. When absent, the electrical simulator calculates mean instantaneous AC power before the chronological environmental calculation. This preliminary pass retains scalar power and one case's waveforms at a time, adding computation. Explicit supplied AC power retains its override behavior. An optional internal-temperature measurement overrides the downstream boundary while preserving the model prediction for validation.

Start a full-profile run with conservative memory settings:

```bash
./bin/trace_pv --topology 3l2s --max-iterations 1 \
  --ngpus 1 --batch-size-limit 1 --pipeline off \
  --wall-time --lifetime --output-dir results/mission_run
```

One iteration means one pass through **accepted records**, not necessarily a complete calendar year. `--rounds` partitions that pass. The default `--max-iterations 0` repeats until the degradation stopping criterion; specify a positive limit for deployment tests. Increase batch size or enable the pipeline only after measuring host RAM and GPU memory use. More GPUs can increase host memory demand.

### Static conditions

```bash
./bin/trace_pv --topology 3l2s --input-mode static \
  --static-temp 25 --static-rh 50 --static-voltage 480 \
  --static-power 300 --static-irradiance 1000 --static-cases 2 \
  --max-iterations 1 --ngpus 1 --batch-size-limit 1 --pipeline off
```

Static mode uses its explicitly provided AC power. Its defaults are 95 °C, 95% RH, 500 V, 300 W, 1000 W/m² and 105,120 cases; override them for a short test. Such high-temperature conditions are stress scenarios, not a validation against field operation.

## 6. Options and outputs

Use `./bin/trace_pv --help` for the complete current option list.

| Option | Purpose |
|---|---|
| `--topology` | Required: `2l2s`, `2l1s`, `3l2s`, `3l1s`; `l` denotes levels and `s` stages |
| `--modulation` | `svm` (default) or `spwm` |
| `--model` | Component/array JSON; defaults to `simulator_inputs/simulation_model/example_simulation_model.json` |
| `--ngpus` | GPU count or `all` (default) |
| `--max-iterations` | Profile repeat limit; `0` is unlimited |
| `--rounds` | Partitions within an iteration; default `1` |
| `--batch-size-limit`, `--pipeline` | Limit batch size and select `on`/`off` overlap |
| `--verbose` | Detailed diagnostic/profiling output |
| `--wall-time`, `--lifetime` | Enable optional summary reports |
| `--output-dir` | Directory for enabled summary reports |
| `--validation-output-dir` | Export model-validation intermediates |
| `--validation-waveform-cases` | `none`, `all`, or comma-separated zero-based indices |

`--wall-time`, `--lifetime`, and `--verbose` are presence-only flags. `--output-dir` does not redirect existing thermal/stressor CSVs or validation exports. Use distinct output directories; selected report filenames are overwritten on rerun. Avoid `all` waveform exports for full-year profiles because they can be large.

Lifetime reports are constant-average-damage projections based on accepted five-minute cases. They do not fill calendar gaps. See [report definitions](docs/OUTPUT_REPORTS.md) and [validation export definitions](docs/MODEL_VALIDATION.md).

## 7. Optional WebUI and SSH access

The UI needs Node.js/npm for its frontend build; the Python HTTP backend uses the standard library:

```bash
(cd webui && npm ci && npm run build)
source setup_env.sh
TRACEPV_WEBUI_HOST=127.0.0.1 TRACEPV_WEBUI_PORT=8080 python3 webui/server.py
```

For persistent deployment, run this backend under the host's service manager. Keep it bound to localhost when using SSH forwarding. From your own computer:

```bash
ssh -p <SSH_PORT> -L 8080:localhost:8080 root@<SERVER_IP>
```

Open <http://localhost:8080>. On Vast images, noninteractive shells may need `. /opt/nvm/nvm.sh` before invoking npm. See [WebUI guide](webui/README.md).

## Troubleshooting and model boundaries

- **`Killed` with no C++ error:** inspect process peak RAM and scheduler/container OOM records; start with batch size 1 and pipeline off. GPU memory alone does not determine host memory consumption.
- **SQLite loader/GLIBC errors:** a copied `sqlite3/lib/libsqlite3.so` may not match the target Linux. Prefer host `libsqlite3-dev`, then rebuild with `make clean` and `make -j2 SQLITE3_PREFIX=/usr`; verify `ldd bin/trace_pv`. Avoid reusing foreign prebuilt libraries.
- **No environment CSV:** provide field inputs or use the committed smoke fixture. Cloning source does not supply all local research datasets.
- **I–V lookup failure:** check panel name, grid coverage and model database path; regenerate the runtime database after changing the panel.
- The existing electrical model consumes the interpolated MPP **voltage**; it does not enforce a nonlinear PV current source. Lookup uses the provided mission temperature, currently ambient, not measured cell temperature. Deployment success does not validate these physical assumptions.
- Detailed capacitor/IGBT thermal models have simplified fallback paths. Check warnings and exported model availability before interpreting results.

## Project layout and further reading

- `src/` — electrical, environmental, thermal and reliability code
- `component_database/` — component JSON sources and generated SQLite databases
- `simulator_inputs/` — model configuration and mission inputs
- `tools/`, `tests/` — database preparation and verification
- `webui/` — React frontend and Python backend
- `IGBT/MATLAB_code/parameters/`, `Capacitor/cpp_standalone/data/` — runtime thermal lookup tables
- [Mission input and I–V changes](docs/mission_input_iv_power_fix.md)
- [Simulation architecture](src/README.md)
- [Input formats](simulator_inputs/README.md)
