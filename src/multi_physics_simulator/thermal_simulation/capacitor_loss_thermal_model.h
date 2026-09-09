#pragma once

#include <string>
#include <vector>

struct CapacitorReferenceThermalResult {
    bool valid = false;
    double loss = 0.0;
    double hotspot_temperature = 0.0;
    double surface_temperature = 0.0;
    double harmonic_extraction_s = 0.0;
    double esr_grid_s = 0.0;
    double loss_grid_s = 0.0;
    double polyfit_s = 0.0;
    double thermal_iteration_s = 0.0;
    std::string message;
};

struct CapacitorReferenceThermalInput {
    const std::vector<double>* time_points = nullptr;
    const std::vector<double>* capacitor_current = nullptr;
    double ambient_temperature = 0.0;
    double rth_surface_ambient = 11.4455;
    double rth_core_surface = 5.05;
    // Multiplier applied to extracted harmonic RMS currents. For a total-bank
    // waveform feeding N identical parallel capacitors, use 1/N. Kept last to
    // preserve positional aggregate initialization used by existing callers.
    double current_scale = 1.0;
};

CapacitorReferenceThermalResult calculate_capacitor_reference_thermal(
    const std::vector<double>& time_points,
    const std::vector<double>& capacitor_current,
    double ambient_temperature,
    double rth_surface_ambient = 11.4455,
    double rth_core_surface = 5.05,
    const std::string& esr_table_path = "Capacitor/cpp_standalone/data/esr_data.csv",
    double current_scale = 1.0);

std::vector<CapacitorReferenceThermalResult> calculate_capacitor_reference_thermal_batch(
    const std::vector<CapacitorReferenceThermalInput>& inputs,
    const std::string& esr_table_path = "Capacitor/cpp_standalone/data/esr_data.csv");
