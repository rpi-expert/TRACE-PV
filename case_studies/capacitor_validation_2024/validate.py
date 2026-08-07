#!/usr/bin/env python3
"""Validate simulated 2024 capacitor hotspot temperatures against units 28-30."""

from pathlib import Path
import json
import sys
from typing import Optional

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


ROOT = Path(__file__).resolve().parents[2]
MISSION = ROOT / "simulator_inputs/mission_profile/year_long_mission_profile_2024.csv"
INTERNAL = ROOT / "internal_source_data"
OUT = Path(__file__).resolve().parent / "outputs"


def metrics(frame: pd.DataFrame) -> dict:
    simulated = frame["capacitor_surface_temperature"]
    error = simulated - frame["SP_cap_temp"]
    return {
        "n": int(len(frame)),
        "coverage_percent": float(100 * len(frame) / frame.attrs["aligned_n"]),
        "mae_c": float(error.abs().mean()),
        "rmse_c": float(np.sqrt(np.mean(error**2))),
        "bias_c": float(error.mean()),
        "correlation": float(simulated.corr(frame["SP_cap_temp"])),
        "measured_mean_c": float(frame["SP_cap_temp"].mean()),
        "simulated_mean_c": float(simulated.mean()),
        "measured_peak_c": float(frame["SP_cap_temp"].max()),
        "simulated_peak_c": float(simulated.max()),
        "peak_error_c": float(simulated.max() - frame["SP_cap_temp"].max()),
    }


def main(simulation_csv: str, output_dir: Optional[str] = None) -> None:
    out = Path(output_dir) if output_dir else OUT
    out.mkdir(parents=True, exist_ok=True)
    mission = pd.read_csv(MISSION, parse_dates=["time"])
    mission = mission[(mission["GHI"] > 0) & (mission["ac_voltage"] > 0)].reset_index(drop=True)
    simulation_path = Path(simulation_csv)
    if simulation_path.is_dir():
        round_files = sorted(simulation_path.glob("capacitor_thermal_round_*_iteration_1.csv"))
        if not round_files:
            raise ValueError(f"No round CSV files found in {simulation_path}")
        simulation = pd.concat((pd.read_csv(path) for path in round_files), ignore_index=True)
        simulation.to_csv(out / "capacitor_thermal_full_year.csv", index=False)
    else:
        simulation = pd.read_csv(simulation_path)
    if "time" in simulation.columns:
        simulation["time"] = pd.to_datetime(simulation["time"])
        simulation = simulation.merge(
            mission[["time", "GHI", "ac_voltage"]], on="time", how="left", validate="one_to_one"
        )
    else:
        if len(mission) != len(simulation):
            raise ValueError(f"Mission/simulation row mismatch: {len(mission)} != {len(simulation)}")
        simulation = pd.concat([mission[["time", "GHI", "ac_voltage"]], simulation], axis=1)

    combined = []
    report = {}
    for path in sorted(INTERNAL.glob("export_data_20240101-20241231_*.csv")):
        unit = path.stem.rsplit("_", 1)[-1]
        measured = pd.read_csv(path, parse_dates=["time"])
        aligned = simulation.merge(measured, on="time", how="inner", validate="one_to_one")
        aligned_n = len(aligned)
        valid = aligned[
            aligned["SP_cap_temp"].between(0.0, 100.0, inclusive="neither")
            & aligned["SP_ambient_temp"].between(-40.0, 80.0)
            & aligned["SP_rh"].between(0.0, 100.0)
        ].copy()
        valid.attrs["aligned_n"] = aligned_n
        valid["unit"] = unit
        report[unit] = metrics(valid)
        first_round = valid[valid["case_index"] < 7655].copy()
        first_round.attrs["aligned_n"] = int((aligned["case_index"] < 7655).sum())
        report[f"{unit}_first_round"] = metrics(first_round)
        combined.append(valid)
        valid.to_csv(out / f"aligned_valid_unit_{unit}.csv", index=False)

    all_valid = pd.concat(combined, ignore_index=True)
    all_valid.attrs["aligned_n"] = sum(len(simulation.merge(pd.read_csv(p, parse_dates=["time"]), on="time")) for p in sorted(INTERNAL.glob("export_data_20240101-20241231_*.csv")))
    report["pooled"] = metrics(all_valid)
    (out / "metrics.json").write_text(json.dumps(report, indent=2) + "\n")
    pd.DataFrame(report).T.to_csv(out / "metrics.csv")

    daily = all_valid.set_index("time").groupby("unit").resample("1D")[["SP_cap_temp", "capacitor_surface_temperature"]].mean().reset_index()
    fig, ax = plt.subplots(figsize=(14, 6))
    sim_daily = daily.groupby("time")["capacitor_surface_temperature"].mean()
    ax.plot(sim_daily.index, sim_daily, color="black", linewidth=1.4, label="Simulated surface")
    for unit, group in daily.groupby("unit"):
        ax.plot(group["time"], group["SP_cap_temp"], linewidth=0.8, alpha=0.75, label=f"Measured unit {unit}")
    ax.set(title="2024 capacitor temperature validation (daily means)", ylabel="Temperature (°C)", xlabel="Date")
    ax.grid(alpha=0.25)
    ax.legend(ncol=4)
    fig.tight_layout()
    fig.savefig(out / "year_long_daily_temperature.png", dpi=180)
    plt.close(fig)

    fig, axes = plt.subplots(1, 3, figsize=(15, 4.8), sharex=True, sharey=True)
    limits = [float(all_valid[["SP_cap_temp", "capacitor_surface_temperature"]].min().min()), float(all_valid[["SP_cap_temp", "capacitor_surface_temperature"]].max().max())]
    for ax, (unit, group) in zip(axes, all_valid.groupby("unit")):
        ax.hexbin(group["SP_cap_temp"], group["capacitor_surface_temperature"], gridsize=55, mincnt=1, bins="log", cmap="viridis")
        ax.plot(limits, limits, "r--", linewidth=1)
        ax.set_title(f"Unit {unit}")
        ax.set_xlabel("Measured capacitor temperature (°C)")
        ax.grid(alpha=0.15)
    axes[0].set_ylabel("Simulated surface temperature (°C)")
    fig.tight_layout()
    fig.savefig(out / "measured_vs_simulated.png", dpi=180)
    plt.close(fig)

    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        raise SystemExit(f"Usage: {sys.argv[0]} PATH_TO_capacitor_thermal_timeseries.csv [OUTPUT_DIR]")
    main(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else None)
