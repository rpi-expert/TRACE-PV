# Selectable TRACE-PV output reports

`--wall-time`, `--lifetime` and `--verbose` are independent presence-only CLI
flags. `--output-dir DIR` selects the summary report directory (default
`results/summary`) and requires `--wall-time` and/or `--lifetime`.

| Selection | Additional files | Console |
|---|---|---|
| No new flags | None | Concise configuration, progress, status, accumulated damage and wall time |
| `--wall-time` | `wall_time.json` | Adds report location |
| `--lifetime` | `lifetime.csv`, `lifetime.json` | Adds report locations and projection qualification |
| `--verbose` | None | Full existing diagnostics/profiling, including debug information |

Flags can be combined with each other and with existing simulation options.
They do not change electrical, thermal, environmental or reliability calculations,
termination rules, cache behavior, or the contents of existing CSV exports.
Warnings and errors remain visible without verbose mode. The WebUI requests
verbose mode explicitly to retain diagnostic-derived cards.

Only selected summary files are written. Reusing a directory overwrites those
named files, not unrelated files. Unselected reports from an earlier run are
not deleted. Use separate run directories when comparing outputs. A report
write failure causes a nonzero exit and an error; it is not silently ignored.
No valid completed reports are promised if the simulator fails before its final
report stage.

## Wall time

`wall_time.json` contains `wall_time_seconds`, `timing_scope`, run stop reason,
accepted case count, represented exposure hours and the case interval.

The timer is `std::chrono::steady_clock`, measured from main entry through
simulation/preprocessing completion, before the final profiling printout and
timing/summary file writes. It is the same duration as the existing
`Total Wall Clock Time` log value, **not the sum of parallel worker/GPU times**.
Final report serialization, process shutdown and external shell overhead are
excluded. Stage percentages are not introduced by this command.

## Lifetime and degradation

The report has eight rows/modes, in this order:

1. Fan electrical, external temperature.
2. Fan electrical, internal temperature.
3. Fan mechanical, external temperature.
4. Fan mechanical, internal temperature.
5. Capacitor.
6. IGBT delta-T / thermo-mechanical cycling.
7. IGBT Arrhenius / RH-voltage-temperature.
8. PCB.

For accepted exposure H hours and accumulated dimensionless damage D:

```text
H = accepted_case_count × case_interval_minutes / 60
projected_lifetime_hours = H / D
projected_lifetime_years = projected_lifetime_hours / 8760
degradation_per_year = D / (H / 8760) = 1 / projected_lifetime_years
```

The simulator's reliability interval remains five minutes. Counts include newly
accepted exposure represented by cached rounds; thermal-only updates do not add
exposure or damage. Partially processed rounds count their processed cases,
not the configured full round. This is represented simulation exposure, not
elapsed calendar time, and does not interpolate gaps in a mission profile.

`projection_method` is `constant_average_damage_extrapolation`. This extrapolates
the run's observed average damage rate, even if the run was deliberately short.
It does not predict subsequent changes caused by degradation-dependent thermal
parameters or validate a short profile as representative of a full year.

`accumulated_damage` is **not** `1/lifetime` unless normalized to a specified time
unit. The explicitly normalized `degradation_per_year` is the reciprocal of the
projected lifetime in years.

| Status | Meaning |
|---|---|
| `right_censored_projection` | D < 1 at the end of the run; projected lifetime is an extrapolation, not a detected failure |
| `failure_detected_at_reporting_boundary` | D ≥ 1; failure was detected by an accepted round boundary |
| `zero_damage` | No accumulated damage; no finite lifetime is asserted |
| `invalid_damage` | Negative or non-finite damage; no valid lifetime or survival claim |
| `no_exposure` | No represented exposure is available for extrapolation |
| `projection_out_of_range` | A valid finite projection cannot be represented numerically |

`detected_failure`, `right_censored`, and `failure_detection_boundary_hours` are
separate from projected lifetime. The boundary records the first accepted round
at which the mode reaches D ≥ 1, in represented exposure hours; it is a reporting
boundary upper bound, **not an exact interpolated failure timestamp**. Parallel
batch processing and early stopping can make this boundary coarse.

`stop_reason=max_iterations_reached` indicates a capped run, not a completed
first-failure simulation. `degradation_limit_reached` indicates the simulator
stopped at its damage threshold. Each mode retains its own detected/censored
status even when another component ends the run.

Unavailable numeric values are JSON `null` and blank CSV fields. Zero damage
may also reflect an unconfigured/skipped model; it is not proof of infinite
physical lifetime. Component configuration and simulator warnings remain part
of interpretation.

## Verification

`make test-reporting` runs host-only tests for report math/serialization, I/O
errors, console filtering/restoration, and the actual command-line parser.
These do not substitute for a CUDA build and simulation smoke test.
