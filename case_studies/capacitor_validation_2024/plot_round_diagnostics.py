#!/usr/bin/env python3
"""Create per-round surface-temperature validation diagnostics."""

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


ROOT = Path(__file__).resolve().parents[2]
ROUNDS = Path(__file__).resolve().parent / "full_year_rounds"
INTERNAL = ROOT / "internal_source_data"
OUT = Path(__file__).resolve().parent / "outputs" / "round_diagnostics"


def score(measured: pd.Series, simulated: pd.Series) -> dict:
    residual = simulated.to_numpy() - measured.to_numpy()
    ss_res = float(np.sum(residual**2))
    centered = measured.to_numpy() - float(measured.mean())
    ss_tot = float(np.sum(centered**2))
    r2_identity = 1.0 - ss_res / ss_tot if ss_tot > 0 else np.nan
    correlation = float(np.corrcoef(measured.to_numpy(), simulated.to_numpy())[0, 1])
    r2_regression = correlation**2
    rmse = float(np.sqrt(np.mean(residual**2)))
    measured_range = float(measured.max() - measured.min())
    nrmse = 100.0 * rmse / measured_range if measured_range > 0 else np.nan
    within_10 = (simulated >= 0.9 * measured) & (simulated <= 1.1 * measured)
    within_20 = (simulated >= 0.8 * measured) & (simulated <= 1.2 * measured)
    within_10_count = int(within_10.sum())
    within_20_count = int(within_20.sum())
    return {
        "n": len(measured),
        "r2_regression": r2_regression,
        "r2_identity": r2_identity,
        "rmse_c": rmse,
        "nrmse_range_percent": nrmse,
        "bias_c": float(np.mean(residual)),
        "within_10_count": within_10_count,
        "within_10_percent": 100.0 * within_10_count / len(measured),
        "within_20_count": within_20_count,
        "within_20_percent": 100.0 * within_20_count / len(measured),
        "measured_min_c": float(measured.min()),
        "measured_max_c": float(measured.max()),
    }


def valid_aligned(simulation: pd.DataFrame, measured_path: Path) -> pd.DataFrame:
    measured = pd.read_csv(measured_path, parse_dates=["time"])
    aligned = simulation.merge(measured, on="time", how="inner", validate="one_to_one")
    return aligned[
        aligned["SP_cap_temp"].between(0.0, 100.0, inclusive="neither")
        & aligned["SP_ambient_temp"].between(-40.0, 80.0)
        & aligned["SP_rh"].between(0.0, 100.0)
    ].copy()


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    measured_paths = sorted(INTERNAL.glob("export_data_20240101-20241231_*.csv"))
    records = []

    for round_path in sorted(ROUNDS.glob("capacitor_thermal_round_*_iteration_1.csv")):
        round_number = int(round_path.name.split("_")[3])
        simulation = pd.read_csv(round_path, parse_dates=["time"])
        aligned_by_unit = []

        for measured_path in measured_paths:
            unit = measured_path.stem.rsplit("_", 1)[-1]
            aligned = valid_aligned(simulation, measured_path)
            stats = score(aligned["SP_cap_temp"], aligned["capacitor_surface_temperature"])
            records.append({"round": round_number, "unit": unit, **stats})
            aligned_by_unit.append((unit, aligned, stats))

        fig, axes = plt.subplots(1, 3, figsize=(16, 5.2), sharex=True, sharey=True)
        all_values = pd.concat(
            [a[["SP_cap_temp", "capacitor_surface_temperature"]] for _, a, _ in aligned_by_unit]
        )
        low = float(all_values.min().min())
        high = float(all_values.max().max())
        pad = max(1.0, 0.04 * (high - low))
        limits = (low - pad, high + pad)
        for ax, (unit, aligned, stats) in zip(axes, aligned_by_unit):
            ax.hexbin(
                aligned["SP_cap_temp"],
                aligned["capacitor_surface_temperature"],
                gridsize=52,
                mincnt=1,
                bins="log",
                cmap="viridis",
            )
            ax.plot(limits, limits, "r--", linewidth=1.2, label="1:1")
            ax.set_title(f"Unit {unit}")
            ax.set_xlabel("Measured surface temperature (°C)")
            ax.set_xlim(limits)
            ax.set_ylim(limits)
            ax.grid(alpha=0.18)
            ax.text(
                0.04,
                0.96,
                f"n = {stats['n']:,}\nR² fit = {stats['r2_regression']:.3f}\nnRMSE = {stats['nrmse_range_percent']:.1f}%\n±10% = {stats['within_10_percent']:.1f}%\n±20% = {stats['within_20_percent']:.1f}%",
                transform=ax.transAxes,
                va="top",
                bbox={"boxstyle": "round", "facecolor": "white", "alpha": 0.88},
            )
        axes[0].set_ylabel("Simulated surface temperature (°C)")
        fig.suptitle(f"Round {round_number}: measured vs simulated capacitor surface temperature")
        fig.tight_layout()
        fig.savefig(OUT / f"round_{round_number}_xy.png", dpi=180)
        plt.close(fig)

        fig, axes = plt.subplots(1, 3, figsize=(16, 5.2), sharex=True, sharey=True)
        residual_limit = max(
            abs(float((a["capacitor_surface_temperature"] - a["SP_cap_temp"]).min()))
            for _, a, _ in aligned_by_unit
        )
        residual_limit = max(
            residual_limit,
            max(
                abs(float((a["capacitor_surface_temperature"] - a["SP_cap_temp"]).max()))
                for _, a, _ in aligned_by_unit
            ),
        )
        residual_limit *= 1.05
        for ax, (unit, aligned, stats) in zip(axes, aligned_by_unit):
            residual = aligned["capacitor_surface_temperature"] - aligned["SP_cap_temp"]
            ax.hexbin(
                aligned["SP_cap_temp"], residual, gridsize=52, mincnt=1, bins="log", cmap="magma"
            )
            ax.axhline(0.0, color="black", linestyle="--", linewidth=1.2)
            ax.set_title(f"Unit {unit}")
            ax.set_xlabel("Measured surface temperature (°C)")
            ax.set_ylim(-residual_limit, residual_limit)
            ax.grid(alpha=0.18)
            ax.text(
                0.04,
                0.96,
                f"n = {stats['n']:,}\nR² fit = {stats['r2_regression']:.3f}\nnRMSE = {stats['nrmse_range_percent']:.1f}%\n±10% = {stats['within_10_percent']:.1f}%\n±20% = {stats['within_20_percent']:.1f}%\nBias = {stats['bias_c']:.1f}°C",
                transform=ax.transAxes,
                va="top",
                bbox={"boxstyle": "round", "facecolor": "white", "alpha": 0.88},
            )
        axes[0].set_ylabel("Residual: simulated − measured (°C)")
        fig.suptitle(f"Round {round_number}: capacitor surface-temperature residuals")
        fig.tight_layout()
        fig.savefig(OUT / f"round_{round_number}_residual.png", dpi=180)
        plt.close(fig)

    metrics = pd.DataFrame(records).sort_values(["round", "unit"])
    metrics.to_csv(OUT / "round_metrics.csv", index=False)
    print(metrics.to_string(index=False))


if __name__ == "__main__":
    main()
