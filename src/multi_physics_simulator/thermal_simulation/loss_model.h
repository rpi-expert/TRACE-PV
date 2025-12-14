#pragma once

#include <vector>

// Capacitor loss calculation result
struct CapacitorLossResult {
    double capacitor_loss;  // Capacitor power loss (W)
};

// Power module loss calculation result
struct PowerModuleLossResult {
    double power_module_loss;  // Power module power loss (W)
};

// Calculate capacitor losses from capacitor current
// Input: I_cap_rms (RMS capacitor current) and ESR (equivalent series resistance)
// Output: Capacitor loss for the case (power_loss = I_rms^2 * ESR)
CapacitorLossResult calculate_capacitor_loss(double I_cap_rms, double esr);

// Calculate power module losses from voltage and current stress
// Input: V_ce and I_c for each sample
// Output: Power module loss for the case
PowerModuleLossResult calculate_power_module_loss(const std::vector<double>& V_ce,
                                                   const std::vector<double>& I_c);

