# 2024 capacitor-temperature validation

These scripts validate simulated capacitor temperatures against local field
measurements for units 28--30.

## Inputs

- `simulator_inputs/mission_profile/year_long_mission_profile_2024.csv`
- Local measurement CSV files documented in `internal_source_data/README.md`
- Simulator-generated capacitor thermal CSV files

## Workflow

1. Run the simulator to generate capacitor thermal results.
2. Run `validate.py` with a result CSV or a directory containing round CSVs.
3. Use `plot_round_diagnostics.py` and `evaluate_daily.py` for additional
   diagnostics.
4. Use `build_mission_with_simulated_ac_power.py` and
   `recalculate_with_ac_power.py` for the AC-power variant.

Generated CSV files, plots, mission profiles, and output directories are
excluded from Git and can be recreated by these scripts.
