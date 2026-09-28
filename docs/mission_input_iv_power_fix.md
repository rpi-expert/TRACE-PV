# Mission input, I–V lookup and environmental power changes

The default model now uses `component_database/runtime_iv_curves.db` and explicit 18-module series / 6-string parallel array scaling. Transfer this database, the updated model JSON, and the source changes to the server before rebuilding. The original component database and raw mission files are unchanged.

## Behavior

- Split environmental/operating CSVs join by timestamp, preserving environmental-file order. Malformed rows no longer misalign the following records. Duplicate operating timestamps fail explicitly.
- Both CSV loaders reject non-finite fields, exact −1 °C temperature sentinels, RH ≤1% or >100%, nonpositive GHI/AC voltage, and invalid optional internal temperatures. Other negative temperatures remain valid. Invalid explicit static inputs fail instead of generating synthetic cases.
- Full-year default input: 47,680 previously retained operating records → 45,934 valid records (1,746 removed). Raw source CSVs are preserved; filtering happens at load time and is logged.
- Runtime SQLite lookup bilinearly interpolates the normalized curves on the actual temperature/GHI grid, calculates the maximum of piecewise-quadratic power, and applies the configured series/parallel scaling. Bad blobs, missing corners, or out-of-range queries fail explicitly; no silent simple-voltage fallback.
- Missing mission AC power is calculated using the electrical simulator and its existing mean instantaneous AC-power calculation before environmental processing. A preliminary pass keeps only scalar power per case and one case's waveforms at a time. The original mission record remains unlabelled as measured power. Explicitly provided power retains its existing override behavior.
- The environmental model receives the chronological calculated power history. Its existing heat/moisture model and power normalization are unchanged. This preliminary pass runs on GPU 0 and adds electrical computation; subsequent stress/thermal simulation still uses the normal configured GPU workers.

## Why a separate database was necessary

The original CS6U-330P STC row stored Voc ≈0.723 V for a module specified at 45.6 V. The offline single-diode solver omits the 72-cell factor in module thermal voltage. Some curve endpoints were also inconsistent with open circuit.

`tools/build_runtime_iv_database.py` generates a separate selected-panel database from the existing component JSON using all 72 cells and a stable implicit single-diode solution. It does not use the old offline generator. STC sanity check: Voc=45.6006 V, Isc=9.4500 A, Pmax=330.3370 W. The generated 1,116 curves cover GHI=0.01–1600 W/m² and temperature=−45–105 °C; all retained yearly records fall inside this grid. These are calculated curves, not measured I–V curves.

Regenerate only if necessary (requires numpy/scipy):

```sh
python3 tools/build_runtime_iv_database.py
```

## Validation and server commands

```sh
make test-mission-iv
make -j2
./bin/trace_pv --topology 3l2s --max-iterations 1 --ngpus 1 --batch-size-limit 1 --pipeline off --verbose
```

CPU tests passed for sentinel/near-zero RH filtering, timestamp alignment across malformed/reordered rows, combined CSV filtering, STC sanity, grid interpolation/edges, array scaling, invalid-database rejection, and all 45,934 yearly IV lookups. Existing environmental-model tests also passed. `main.cpp` passed a host C++ syntax check using temporary CUDA/OpenMP declarations; that is not a CUDA build or GPU execution. This Mac has no nvcc. Subsequent Linux/CUDA deployment and GPU checks are recorded in [the deployment report](deployment_20260918/REPORT.md). The small batch command limits memory pressure; it is not evidence that the earlier server `Killed` condition is resolved.

## Model limits

The mission temperature column is ambient temperature; lookup uses that temperature as provided, not measured cell temperature. The existing electrical model is voltage-source based: the lookup supplies MPP voltage; the returned MPP current is not enforced as a nonlinear PV current source. AC power is therefore the electrical simulator's result, not an assumed efficiency times PV MPP power. This change does not establish field-vs-model accuracy or fix that underlying current-source limitation.

The environmental history remains indexed by retained samples, as before; removing invalid rows does not fill missing clock time. The preliminary environmental load uses initial electrical parameters and is not recomputed for later degradation iterations. New internal temperature/RH extrema require the server run; the input filtering does not impose an artificial floor on predicted RH.
