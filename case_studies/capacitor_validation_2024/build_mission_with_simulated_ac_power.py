#!/usr/bin/env python3
"""Build a mission CSV using AC power calculated by the baseline simulation."""

from pathlib import Path

import pandas as pd


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
MISSION = ROOT / "simulator_inputs/mission_profile/year_long_mission_profile_2024.csv"
SIMULATION = HERE / "outputs/capacitor_thermal_full_year.csv"
OUTPUT = HERE / "mission_profiles/year_long_mission_profile_2024_simulated_ac_power.csv"


def main() -> None:
    mission = pd.read_csv(MISSION, parse_dates=["time"])
    simulation = pd.read_csv(SIMULATION, parse_dates=["time"], usecols=["time", "ac_power"])
    simulation["ac_power"] = simulation["ac_power"].abs()
    combined = mission.merge(simulation, on="time", how="inner", validate="one_to_one")
    combined = combined[["time", "ambient_temperature", "rh", "GHI", "ac_voltage", "ac_power"]]
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    combined.to_csv(OUTPUT, index=False, date_format="%Y-%m-%d %H:%M:%S")
    print(f"rows={len(combined)} output={OUTPUT}")
    print(combined["ac_power"].describe().to_string())


if __name__ == "__main__":
    main()
