#!/usr/bin/env python3
"""Recalculate DDM internal and capacitor temperatures from simulated AC power."""

from pathlib import Path

import numpy as np
import pandas as pd


HERE = Path(__file__).resolve().parent
SOURCE = HERE / "outputs" / "capacitor_thermal_full_year.csv"
OUT_DIR = HERE / "outputs_ac_power"
OUTPUT = OUT_DIR / "capacitor_thermal_full_year_ac_power.csv"

RATED_POWER_W = 10_000.0
EWMA_SPAN = 20.0
TEMPERATURE_GAIN_C_PER_W = 0.05
RTH_SURFACE_K_PER_W = 8.3391


def clamp(value: float, low: float, high: float) -> float:
    return min(max(value, low), high)


def main() -> None:
    data = pd.read_csv(SOURCE)
    load = data["ac_power"].abs().to_numpy(dtype=float)
    rated_load = float(np.nanmax(load))
    if not np.isfinite(rated_load) or rated_load <= 0:
        raise ValueError("Simulated AC power has no finite nonzero magnitude")

    alpha = 2.0 / (EWMA_SPAN + 1.0)
    ewma_num = 0.0
    ewma_den = 0.0
    internal = np.empty(len(data), dtype=float)
    for index, (ambient, power) in enumerate(zip(data["ambient_temperature"], load)):
        ratio = clamp(float(power) / rated_load, 0.0, 1.5)
        efficiency = clamp(0.98 - 0.08 * (1.0 - ratio) ** 4, 0.85, 0.99)
        power_out = ratio * RATED_POWER_W
        waste_heat = power_out / efficiency - power_out
        ewma_num = waste_heat + (1.0 - alpha) * ewma_num
        ewma_den = 1.0 + (1.0 - alpha) * ewma_den
        filtered_heat = ewma_num / ewma_den if ewma_den > 0 else waste_heat
        internal[index] = clamp(float(ambient) + TEMPERATURE_GAIN_C_PER_W * filtered_heat, -50.0, 100.0)

    data["original_internal_temperature"] = data["internal_temperature"]
    data["original_capacitor_surface_temperature"] = data["capacitor_surface_temperature"]
    data["original_capacitor_hotspot_temperature"] = data["capacitor_hotspot_temperature"]
    data["ac_power_thermal_load"] = load
    data["internal_temperature"] = internal
    data["capacitor_surface_temperature"] = (
        internal + data["capacitor_loss"].to_numpy() * RTH_SURFACE_K_PER_W
    )
    data["capacitor_hotspot_temperature"] = (
        internal + data["capacitor_loss"].to_numpy() * 16.6783
    )

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    data.to_csv(OUTPUT, index=False)
    print(f"rows={len(data)} rated_load_magnitude={rated_load:.6f} output={OUTPUT}")
    print(data[["ac_power", "ac_power_thermal_load", "internal_temperature", "capacitor_surface_temperature"]].describe())


if __name__ == "__main__":
    main()
