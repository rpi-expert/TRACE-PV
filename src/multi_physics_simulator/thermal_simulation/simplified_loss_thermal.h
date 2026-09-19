#pragma once

#include <string>

// Host-only approximations still used by the explicit fast mode, the existing
// reference-result fallback, and capacitor thermal-only refresh. These are NOT
// the detailed loss/thermal models. The detailed implementations live in
// capacitor_loss_thermal_model.cpp and igbt_loss_thermal_model.cpp, backed by
// capacitor_reference_gpu.cu and igbt_reference_gpu.cu.
//
// Keep only live helpers here. The unused legacy CUDA kernels and the invalid
// V_ce/I_c-only IGBT loss API have been removed.

struct CapacitorLossResult {
    double capacitor_loss;  // W
};

// The main simulation fills this from the detailed IGBT model. No simplified
// IGBT loss estimator is provided: unavailable loss must stay explicitly invalid.
struct PowerModuleLossResult {
    double power_module_loss = 0.0;  // W; usable only when valid is true
    bool valid = false;
    std::string message;
};

struct CapacitorThermalResult {
    double capacitor_hotspot_temperature;  // degrees C
    double capacitor_surface_temperature;  // degrees C
};

struct PowerModuleThermalResult {
    double junction_temperature;  // degrees C
};

// Caller supplies topology-adjusted ESR; preserve the existing negative-input
// behavior of the simplified path.
inline CapacitorLossResult calculate_capacitor_loss(double current_rms, double esr) {
    return {(current_rms < 0.0 || esr < 0.0) ? 0.0 : current_rms * current_rms * esr};
}

inline CapacitorThermalResult calculate_capacitor_thermal(
    double capacitor_loss,
    double internal_temperature,
    double rth_amb,
    double rth_surf) {
    return {internal_temperature + capacitor_loss * rth_amb,
            internal_temperature + capacitor_loss * rth_surf};
}

// Legacy AC-power approximation, NOT a loss-driven RC junction-temperature
// calculation. Retained only to preserve existing fast/fallback behavior.
inline PowerModuleThermalResult calculate_power_module_thermal(
    double ac_power,
    double ambient_temperature) {
    return {ambient_temperature + (ac_power / 30000.0) * (100.0 - 45.0)};
}
