#!/usr/bin/env python3
"""Evaluate formal AC-power rerun surface temperatures for each day and unit."""

from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.colors import LogNorm
import numpy as np
import pandas as pd


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SIMULATION = HERE / "outputs_ac_power_rerun/capacitor_thermal_full_year.csv"
INTERNAL = ROOT / "internal_source_data"
OUT = HERE / "outputs_ac_power_rerun/daily_evaluation"


def metrics(group: pd.DataFrame) -> pd.Series:
    measured = group["SP_cap_temp"].to_numpy(dtype=float)
    simulated = group["capacitor_surface_temperature"].to_numpy(dtype=float)
    residual = simulated - measured
    rmse = float(np.sqrt(np.mean(residual**2)))
    measured_range = float(np.max(measured) - np.min(measured))
    if len(group) >= 2 and np.std(measured) > 0 and np.std(simulated) > 0:
        r2 = float(np.corrcoef(measured, simulated)[0, 1] ** 2)
    else:
        r2 = np.nan
    within_10 = (simulated >= 0.9 * measured) & (simulated <= 1.1 * measured)
    within_20 = (simulated >= 0.8 * measured) & (simulated <= 1.2 * measured)
    return pd.Series(
        {
            "n": len(group),
            "mae_c": float(np.mean(np.abs(residual))),
            "rmse_c": rmse,
            "bias_c": float(np.mean(residual)),
            "r2_regression": r2,
            "nrmse_range_percent": 100.0 * rmse / measured_range if measured_range > 0 else np.nan,
            "within_10_count": int(np.sum(within_10)),
            "within_10_percent": float(100.0 * np.mean(within_10)),
            "within_20_count": int(np.sum(within_20)),
            "within_20_percent": float(100.0 * np.mean(within_20)),
            "measured_mean_c": float(np.mean(measured)),
            "simulated_mean_c": float(np.mean(simulated)),
            "measured_min_c": float(np.min(measured)),
            "measured_max_c": float(np.max(measured)),
            "simulated_min_c": float(np.min(simulated)),
            "simulated_max_c": float(np.max(simulated)),
        }
    )


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    simulation = pd.read_csv(SIMULATION, parse_dates=["time"])
    aligned_units = []
    for measured_path in sorted(INTERNAL.glob("export_data_20240101-20241231_*.csv")):
        unit = measured_path.stem.rsplit("_", 1)[-1]
        measured = pd.read_csv(measured_path, parse_dates=["time"])
        aligned = simulation.merge(measured, on="time", how="inner", validate="one_to_one")
        aligned = aligned[
            aligned["SP_cap_temp"].between(0.0, 100.0, inclusive="neither")
            & aligned["SP_ambient_temp"].between(-40.0, 80.0)
            & aligned["SP_rh"].between(0.0, 100.0)
        ].copy()
        aligned["unit"] = unit
        aligned["date"] = aligned["time"].dt.floor("D")
        aligned_units.append(aligned)

    combined = pd.concat(aligned_units, ignore_index=True)
    daily_unit = (
        combined.groupby(["date", "unit"], observed=True)
        .apply(metrics, include_groups=False)
        .reset_index()
        .sort_values(["date", "unit"])
    )
    daily_pooled = (
        combined.groupby("date", observed=True)
        .apply(metrics, include_groups=False)
        .reset_index()
        .sort_values("date")
    )
    daily_unit.to_csv(OUT / "daily_metrics_by_unit.csv", index=False)
    daily_pooled.to_csv(OUT / "daily_metrics_pooled.csv", index=False)
    daily_unit[["date", "unit", "n", "r2_regression", "nrmse_range_percent"]].to_csv(
        OUT / "daily_accuracy_r2_nrmse_by_unit.csv", index=False
    )
    daily_pooled[["date", "n", "r2_regression", "nrmse_range_percent"]].to_csv(
        OUT / "daily_accuracy_r2_nrmse_pooled.csv", index=False
    )

    full_dates = pd.date_range("2024-01-01", "2024-12-31", freq="D")
    units = sorted(daily_unit["unit"].astype(str).unique())
    heatmap_specs = [
        ("within_10_percent", "Within ±10% (%)", 0.0, 100.0, None),
        ("within_20_percent", "Within ±20% (%)", 0.0, 100.0, None),
        ("r2_regression", "Regression R²", 0.0, 1.0, None),
        ("nrmse_range_percent", "nRMSE (% measured range)", None, None, "log"),
    ]
    fig, axes = plt.subplots(4, 1, figsize=(18, 10), sharex=True)
    month_starts = pd.date_range(full_dates.min(), full_dates.max(), freq="MS")
    month_positions = [int((date - full_dates.min()).days) for date in month_starts]
    for ax, (column, title, vmin, vmax, scale) in zip(axes, heatmap_specs):
        matrix = (
            daily_unit.assign(unit=daily_unit["unit"].astype(str))
            .pivot(index="unit", columns="date", values=column)
            .reindex(index=units, columns=full_dates)
            .to_numpy(dtype=float)
        )
        cmap = plt.get_cmap("viridis").copy()
        cmap.set_bad("lightgray")
        if scale == "log":
            positive = matrix[np.isfinite(matrix) & (matrix > 0)]
            norm = LogNorm(vmin=max(1.0, float(np.min(positive))), vmax=float(np.max(positive)))
            image = ax.imshow(matrix, aspect="auto", interpolation="nearest", cmap=cmap, norm=norm)
        else:
            image = ax.imshow(
                matrix, aspect="auto", interpolation="nearest", cmap=cmap, vmin=vmin, vmax=vmax
            )
        ax.set_yticks(range(len(units)), [f"Unit {unit}" for unit in units])
        ax.set_title(title, loc="left")
        fig.colorbar(image, ax=ax, pad=0.01, fraction=0.025)
    axes[-1].set_xticks(month_positions, [date.strftime("%b") for date in month_starts])
    axes[-1].set_xlabel("Date in 2024")
    fig.suptitle("Daily capacitor surface-temperature accuracy heatmaps")
    fig.tight_layout()
    fig.savefig(OUT / "daily_accuracy_heatmaps.png", dpi=180)
    plt.close(fig)

    for column, title, vmin, vmax, scale in heatmap_specs:
        matrix = (
            daily_unit.assign(unit=daily_unit["unit"].astype(str))
            .pivot(index="unit", columns="date", values=column)
            .reindex(index=units, columns=full_dates)
            .to_numpy(dtype=float)
        )
        cmap = plt.get_cmap("viridis").copy()
        cmap.set_bad("lightgray")
        fig, ax = plt.subplots(figsize=(22, 5.5))
        if scale == "log":
            positive = matrix[np.isfinite(matrix) & (matrix > 0)]
            norm = LogNorm(vmin=max(1.0, float(np.min(positive))), vmax=float(np.max(positive)))
            image = ax.imshow(matrix, aspect="auto", interpolation="nearest", cmap=cmap, norm=norm)
        else:
            image = ax.imshow(
                matrix, aspect="auto", interpolation="nearest", cmap=cmap, vmin=vmin, vmax=vmax
            )
        ax.set_yticks(range(len(units)), [f"Unit {unit}" for unit in units], fontsize=15)
        ax.set_xticks(month_positions, [date.strftime("%b") for date in month_starts], fontsize=14)
        ax.set_xlabel("Date in 2024", fontsize=16)
        ax.set_title(f"Daily capacitor surface-temperature accuracy: {title}", fontsize=20, pad=14)
        colorbar = fig.colorbar(image, ax=ax, pad=0.012, fraction=0.025)
        colorbar.ax.tick_params(labelsize=13)
        colorbar.set_label(title, fontsize=15)
        fig.tight_layout()
        filename = {
            "within_10_percent": "daily_heatmap_within_10_percent.png",
            "within_20_percent": "daily_heatmap_within_20_percent.png",
            "r2_regression": "daily_heatmap_r2.png",
            "nrmse_range_percent": "daily_heatmap_nrmse.png",
        }[column]
        fig.savefig(OUT / filename, dpi=240, bbox_inches="tight")
        plt.close(fig)

    calendar_dir = OUT / "calendar_heatmaps_by_unit"
    calendar_dir.mkdir(parents=True, exist_ok=True)
    month_labels = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
    calendar_data = daily_unit.copy()
    calendar_data["month"] = calendar_data["date"].dt.month
    calendar_data["day"] = calendar_data["date"].dt.day
    global_positive_nrmse = calendar_data.loc[
        np.isfinite(calendar_data["nrmse_range_percent"]) & (calendar_data["nrmse_range_percent"] > 0),
        "nrmse_range_percent",
    ]
    for unit in units:
        unit_data = calendar_data[calendar_data["unit"].astype(str) == unit]
        for column, title, vmin, vmax, scale in heatmap_specs:
            matrix = (
                unit_data.pivot(index="day", columns="month", values=column)
                .reindex(index=range(1, 32), columns=range(1, 13))
                .to_numpy(dtype=float)
            )
            cmap = plt.get_cmap("viridis").copy()
            cmap.set_bad("lightgray")
            fig, ax = plt.subplots(figsize=(13, 10.5))
            if scale == "log":
                norm = LogNorm(
                    vmin=max(1.0, float(global_positive_nrmse.min())),
                    vmax=float(global_positive_nrmse.max()),
                )
                image = ax.imshow(matrix, aspect="auto", interpolation="nearest", cmap=cmap, norm=norm)
            else:
                image = ax.imshow(
                    matrix, aspect="auto", interpolation="nearest", cmap=cmap, vmin=vmin, vmax=vmax
                )
            ax.set_xticks(range(12), month_labels, fontsize=14)
            ax.set_yticks(range(31), range(1, 32), fontsize=11)
            ax.set_xlabel("Month", fontsize=16)
            ax.set_ylabel("Day of month", fontsize=16)
            ax.set_title(f"Unit {unit}: {title}", fontsize=20, pad=14)
            colorbar = fig.colorbar(image, ax=ax, pad=0.02, fraction=0.04)
            colorbar.ax.tick_params(labelsize=12)
            colorbar.set_label(title, fontsize=14)
            fig.tight_layout()
            metric_name = {
                "within_10_percent": "within_10_percent",
                "within_20_percent": "within_20_percent",
                "r2_regression": "r2",
                "nrmse_range_percent": "nrmse",
            }[column]
            fig.savefig(
                calendar_dir / f"unit_{unit}_{metric_name}.png",
                dpi=240,
                bbox_inches="tight",
            )
            plt.close(fig)

    plot_specs = [
        ("mae_c", "Daily MAE (°C)"),
        ("rmse_c", "Daily RMSE (°C)"),
        ("bias_c", "Daily bias (°C)"),
        ("r2_regression", "Daily regression R²"),
        ("nrmse_range_percent", "Daily nRMSE (% measured range)"),
        ("within_10_percent", "Points within ±10% (%)"),
        ("within_20_percent", "Points within ±20% (%)"),
    ]
    fig, axes = plt.subplots(4, 2, figsize=(16, 16), sharex=True)
    axes = axes.ravel()
    for ax, (column, label) in zip(axes, plot_specs):
        for unit, group in daily_unit.groupby("unit"):
            ax.plot(group["date"], group[column], linewidth=0.8, alpha=0.85, label=f"Unit {unit}")
        ax.set_ylabel(label)
        ax.grid(alpha=0.22)
    axes[len(plot_specs)].axis("off")
    axes[0].legend(ncol=3)
    fig.suptitle("Daily capacitor surface-temperature validation: formal AC-power rerun")
    fig.tight_layout()
    fig.savefig(OUT / "daily_metric_trends.png", dpi=180)
    plt.close(fig)

    print(f"daily unit rows={len(daily_unit)}, pooled days={len(daily_pooled)}")
    print(daily_unit.groupby("unit")[["mae_c", "rmse_c", "bias_c", "r2_regression", "within_10_percent", "within_20_percent"]].mean().to_string())


if __name__ == "__main__":
    main()
