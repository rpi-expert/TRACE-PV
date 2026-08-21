#pragma once

#include <array>
#include <vector>

struct IgbtGpuTimeValue {
    std::vector<double> t;
    std::vector<double> y;
};

struct IgbtGpuDeviceInput {
    IgbtGpuTimeValue fundamental_i;
    IgbtGpuTimeValue rising_i;
    IgbtGpuTimeValue falling_v;
    IgbtGpuTimeValue falling_i;
    IgbtGpuTimeValue rising_v;
};

struct IgbtGpuGrid2D {
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> z;
    int x_n = 0;
    int y_n = 0;
};

struct IgbtGpuLossTable {
    int num_temps = 0;
    int num_windows = 0;
    std::vector<double> temperatures;
    std::vector<double> losses;
};

struct IgbtGpuThermalResult {
    bool valid = false;
    double junction_temperature = 0.0;
    double igbt1_temperature = 0.0;
    double igbt2_temperature = 0.0;
    double diode1_temperature = 0.0;
    double diode2_temperature = 0.0;
    double case_temperature = 0.0;
    double heatsink_temperature = 0.0;
    double average_total_loss = 0.0;
};

bool compute_igbt_loss_tables_gpu_batch(
    const std::vector<std::array<IgbtGpuDeviceInput, 4>>& batch_devices,
    const std::array<IgbtGpuGrid2D, 4>& conduction_grids,
    const std::array<IgbtGpuGrid2D, 4>& turn_on_or_recovery_grids,
    const std::array<IgbtGpuGrid2D, 2>& turn_off_grids,
    double tavg,
    double tsim,
    std::vector<std::array<IgbtGpuLossTable, 4>>& batch_loss_tables,
    std::array<double, 4>& elapsed_s);

bool run_igbt_thermal_gpu_batch(
    const std::vector<std::array<IgbtGpuLossTable, 4>>& batch_loss_tables,
    const std::vector<double>& ambient_temperatures,
    double loss_tavg,
    double thermal_dt,
    double thermal_end_time_s,
    std::vector<IgbtGpuThermalResult>& results,
    double& elapsed_s);

bool run_igbt_loss_thermal_fused_gpu_batch(
    const std::vector<std::array<IgbtGpuDeviceInput, 4>>& batch_devices,
    const std::array<IgbtGpuGrid2D, 4>& conduction_grids,
    const std::array<IgbtGpuGrid2D, 4>& turn_on_or_recovery_grids,
    const std::array<IgbtGpuGrid2D, 2>& turn_off_grids,
    const std::vector<double>& ambient_temperatures,
    double loss_tavg,
    double loss_tsim,
    double thermal_dt,
    double thermal_end_time_s,
    std::vector<IgbtGpuThermalResult>& results,
    std::array<double, 4>& loss_elapsed_s,
    double& thermal_elapsed_s);
