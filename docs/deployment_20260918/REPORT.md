# TRACE-PV deployment verification — 2026-09-18

Deployment directory: `/workspace/TRACE-PV` on the requested GPU server. The deployed source is a snapshot of the local working tree, with the current GitHub database-initializer fix retained; it is not represented as a clean Git commit.

## Environment and results

| Check | Result |
|---|---|
| GPU | NVIDIA Tesla V100-SXM2, 32 GB, compute capability 7.0 |
| Compiler | CUDA toolkit 12.8.93, GCC 13.3.0 |
| Python / SQLite | Python 3.12.3 / system SQLite 3.45.1 |
| Clean native build | `make -j2`, passed |
| Input and I–V tests | Passed; all 45,934 retained yearly records covered |
| CLI/reporting tests | Passed, including 14 Python CLI/integration tests |
| Simplified thermal compatibility | Passed |
| Database setup from empty files | Passed; 12 components, 1,116 module curves |
| Two-case actual GPU test | Exit 0; 2.924 s; peak RSS 301,880 KiB (294.8 MiB) |
| Same GPU test using freshly generated databases | Passed |
| 95 °C static GPU test, one case | Passed |
| WebUI frontend | `npm ci` and `npm run build`, passed |
| WebUI backend | Supervisor `tracepv-webui`, localhost port 8081 |
| HTTP checks | `/` returns 200; `/api/health` reports simulator present |

The two-case mission fixture contains no AC power. The run logged successful I–V lookup and completed its electrical-power prepass. Predicted internal temperature was 30.66–31.66 °C and RH was 35.94–36.69%. No missing-power or missing-IV warnings were emitted. These are smoke-test outputs, not annual field-validation results.

The complete annual dataset was checked for cleaning and I–V coverage; a full-year GPU/reliability run was not performed. No long-running GPU simulation is intentionally left running.

## Access and operations

The existing Jupyter service occupies server port 8080 and was preserved. Forward local port 8080 to **server port 8081** for TRACE-PV, using the SSH host and port supplied by the user. Visit `http://localhost:8080` locally.

```bash
cd /workspace/TRACE-PV
source setup_env.sh
supervisorctl status tracepv-webui
# If an intentional restart is needed:
supervisorctl restart tracepv-webui
```

Supervisor configuration: `/etc/supervisor/conf.d/tracepv-webui.conf`.
Launcher: `/opt/supervisor-scripts/tracepv-webui.sh`.
The UI listens only on `127.0.0.1:8081`.

Server evidence is retained under `deployment_logs/`, including `build.log`, `host_tests.log`, `database_setup.log`, `gpu_smoke.log`, `gpu_smoke_metrics.json`, `clean_database_gpu_smoke.log`, `static_95_gpu_smoke.log`, `python_freeze.txt`, and frontend install/build logs. Simulator reports are under `results/deployment_smoke/`.

## README audit and corrections

Compared with the README at GitHub revision `c6f2022a2420e2f6618afd46389fb18f2b8f68a6`:

1. Reordered setup so the environment is created before activation, and all commands remain at the repository root.
2. Replaced Python 3.8-era package pins with Python 3.10+ compatible ranges, including the required SciPy dependency. Actual server versions are recorded in `python_freeze.txt`.
3. Retained the upstream initializer's full inverter/grid loading and added `--skip-legacy-pv`, separating component initialization from corrected runtime I–V generation.
4. Extended the corrected I–V grid to 105 °C so the existing 95 °C static default is covered; invalid out-of-grid inputs still fail explicitly.
5. Added a committed two-case fixture without AC power and a bounded GPU smoke command before annual runs.
6. Documented raw-data availability: GitHub does not include the local environmental dataset or the detailed thermal lookup assets. The server package includes them. A source-only clone must obtain these assets before detailed-model testing.
7. Documented RH/temperature cleaning, timestamp alignment, model/array configuration, memory controls, outputs and physical-model limitations.
8. Replaced the generic Create React App README with the actual TRACE-PV backend, build and SSH workflow.

At deployment verification time, README and code updates existed locally and on the deployment host, with GitHub publication pending. This report is included with the subsequent source publication; Git history records that publication. Git push uses the system Git credential helper independently of GitHub CLI login.
