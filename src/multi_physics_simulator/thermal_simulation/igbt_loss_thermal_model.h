#pragma once

#include "multi_physics_simulator/electrical_simulation/a2s_gpu.h"
#include "multi_physics_simulator/thermal_simulation/igbt_reference_gpu.h"
#include "simulation_params.h"

#include <array>
#include <string>
#include <vector>

struct IgbtReferenceThermalResult {
    bool valid = false;
    double junction_temperature = 0.0;
    double igbt1_temperature = 0.0;
    double igbt2_temperature = 0.0;
    double diode1_temperature = 0.0;
    double diode2_temperature = 0.0;
    double case_temperature = 0.0;
    double heatsink_temperature = 0.0;
    double average_total_loss = 0.0;
    double parameter_lookup_s = 0.0;
    double input_build_s = 0.0;
    double loss_igbt1_s = 0.0;
    double loss_igbt2_s = 0.0;
    double loss_diode1_s = 0.0;
    double loss_diode2_s = 0.0;
    double thermal_rc_s = 0.0;
    std::string message;
};

struct IgbtReferenceThermalInput {
    const UnifiedOutputs* outputs = nullptr;
    const SimulationParameters* params = nullptr;
    double ambient_temperature = 0.0;
};

struct IgbtReferenceCachedThermalInput {
    const std::array<IgbtGpuDeviceInput, 4>* devices = nullptr;
    double tavg = 0.0;
    double tsim = 0.0;
    double ambient_temperature = 0.0;
};

bool build_igbt_reference_cached_input(
    const UnifiedOutputs& outputs,
    const SimulationParameters& params,
    std::array<IgbtGpuDeviceInput, 4>& devices,
    double& tavg,
    double& tsim,
    std::string& message);

IgbtReferenceThermalResult calculate_igbt_reference_thermal(
    const UnifiedOutputs& outputs,
    const SimulationParameters& params,
    double ambient_temperature,
    const std::string& parameter_dir = "IGBT/MATLAB_code/parameters",
    double thermal_end_time_s = 20.0,
    double thermal_step_s = 0.0);

std::vector<IgbtReferenceThermalResult> calculate_igbt_reference_thermal_batch(
    const std::vector<IgbtReferenceThermalInput>& inputs,
    const std::string& parameter_dir = "IGBT/MATLAB_code/parameters",
    double thermal_end_time_s = 20.0,
    double thermal_step_s = 0.0);

std::vector<IgbtReferenceThermalResult> calculate_igbt_reference_thermal_batch_cached(
    const std::vector<IgbtReferenceCachedThermalInput>& inputs,
    const std::string& parameter_dir = "IGBT/MATLAB_code/parameters",
    double thermal_end_time_s = 20.0,
    double thermal_step_s = 0.0);
