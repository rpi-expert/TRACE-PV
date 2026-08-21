#pragma once

#include "a2s_gpu.h"
#include "simulation_params.h"
#include <vector>

// Stress calculation results for a single case
struct StressResults {
    std::vector<double> V_ce;  // Voltage stress on Phase A Top IGBT (V)
    std::vector<double> I_c;   // Current stress on Phase A Top IGBT (A)
    std::vector<double> I_cap; // DC Link capacitor ripple current (A)
    double I_cap_rms;          // RMS value of capacitor current (A)
};

// AC power calculation results
struct ACPowerResults {
    std::vector<double> p_AC_instantaneous;  // Instantaneous AC power (W)
    double P_AC_average;                     // Average active AC power (W)
};

// Calculate stress waveforms from a2s simulation outputs
// Based on MATLAB single_stage.m and two_stage.m
StressResults calculate_stress(const UnifiedOutputs& outputs, 
                               const SimulationParameters& params);

// Calculate AC power from I2 (grid-side inductor currents) and Vc (filter capacitor voltages)
// For 2-level and 3-level topologies:
//   State indices: i_L2a=3, i_L2b=4, i_L2c=5, v_Ca=6, v_Cb=7, v_Cc=8
//   p_export(t) = -(v_Ca(t) * i_L2a(t) + v_Cb(t) * i_L2b(t) + v_Cc(t) * i_L2c(t))
// AC power uses the public convention: positive means power exported by the
// inverter to the grid.
ACPowerResults calculate_ac_power(const UnifiedOutputs& outputs,
                                  const SimulationParameters& params);

// Convert instantaneous power to a single equivalent value (average power)
// This is used for thermal model input
double calculate_equivalent_ac_power(const std::vector<double>& p_AC_instantaneous);
