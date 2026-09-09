# Model-validation intermediate export

The intermediate export is opt-in so normal long-running simulations do not
pay the storage and CSV I/O cost of device-level waveforms.

## Quick start

```bash
./bin/trace_pv --topology 3l2s --input-mode static \
  --static-cases 1 --max-iterations 1 \
  --validation-output-dir results/model_validation \
  --validation-waveform-cases 0
```

`--validation-waveform-cases` accepts `none`, `all`, or a comma-separated list
of zero-based case indices such as `0,12,25`. It defaults to case `0` whenever
the validation export is enabled. Invalid or out-of-range indices stop before
simulation begins with an explanatory error.

## Output layout

```text
results/model_validation/
├── a2s_core/
│   └── case_0.csv
└── iteration_1/
    └── intermediate_summary_round_1.csv
```

Each `a2s_core/case_<index>.csv` is the requested matrix:

```text
time_s,peak_voltage_v,peak_current_a
```

The voltage and current columns are the existing A2S-derived `V_ce` and `I_c`
sample waveforms for the phase-A top (or outer-top) switch. They retain every
sample; they are not a window-level maximum reduction. Both stress vectors use
the same index and time vector. Each selected matrix is written once from the
first electrical run because later degradation iterations reuse that waveform.

Each `intermediate_summary_round_<round>.csv` contains one row per case in that
round. `case_processed=1` means the case reached the completed loss/thermal
stage. If an early-stop condition leaves a case unprocessed, its row keeps the
known time and environmental inputs; numeric model values remain empty and
their count/validity/provenance flags remain zero.
Important groups are:

- Average Model: `average_model_duty_*` and `x_dq_ss_*`. Stage 1 has six dq
  states; its boost-current and DC-link columns are intentionally empty. Stage
  2 has all eight states.
- Processed A2S: `capacitor_rms_current_a`, the per-device RMS stress for the
  model's five parallel capacitors. The internal `I_cap` waveform remains total
  capacitor-bank current; the parallel-device division is applied once to RMS.
  The reference harmonic-loss path applies the same `1/5` current scale after
  either CPU or GPU harmonic extraction.
- Loss and temperature: `capacitor_loss_w`,
  `inverter_average_total_loss_w`, capacitor surface/hotspot temperatures, and
  IGBT junction temperature. `inverter_average_total_loss_valid=0` leaves the
  loss cell empty because the fallback path cannot derive a physical inverter
  loss.
- Environment: ambient T/RH plus predicted and used internal T/RH. A supplied
  internal-temperature boundary has source `provided`; otherwise it is
  `predicted`. The current input schema has no measured internal-RH field. When
  provided internal temperature overrides the prediction, used internal RH is
  recomputed from that temperature and the DDM model's unrounded internal dew
  point; its source is `recomputed_from_internal_dew_point`.
- Provenance: loss and final-temperature reference flags are separate for the
  capacitor and IGBT. If a degradation bucket changes after a round, the final
  temperatures are adjusted without rerunning the electrical calculation;
  `*_temperature_adjusted_after_degradation=1` identifies those values and the
  corresponding `*_reference_temperature_used` flag is `0`.

Unavailable numeric values are empty CSV cells, not zero. CSV files use
`max_digits10` precision so finite double values round-trip without avoidable
text-conversion loss.
