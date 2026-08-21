#pragma once

#include <string>
#include <vector>

// Capacitor loss calculation result
struct CapacitorLossResult {
    double capacitor_loss;  // Capacitor power loss (W)
};

// Power module loss calculation result
struct PowerModuleLossResult {
    double power_module_loss = 0.0;  // Power module power loss (W)
    bool valid = false;
    std::string message;
};

// Calculate capacitor losses from capacitor current
// Input: I_cap_rms (RMS capacitor current) and ESR (equivalent series resistance)
// Output: Capacitor loss for the case (power_loss = I_rms^2 * ESR)
CapacitorLossResult calculate_capacitor_loss(double I_cap_rms, double esr);

// Legacy simplified interface. Exact IGBT loss requires the reference model
// inputs used by calculate_igbt_reference_thermal_batch(), not only V_ce/I_c.
PowerModuleLossResult calculate_power_module_loss(const std::vector<double>& V_ce,
                                                   const std::vector<double>& I_c);
