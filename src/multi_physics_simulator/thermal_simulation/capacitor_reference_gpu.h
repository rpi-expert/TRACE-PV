#pragma once

#include <utility>
#include <vector>

bool calculate_capacitor_harmonics_gpu(
    const std::vector<double>& time,
    const std::vector<double>& current,
    std::vector<std::pair<double, double>>& harmonics,
    double& elapsed_s);

bool calculate_capacitor_esr_grid_gpu(
    const std::vector<std::pair<double, double>>& harmonics,
    const std::vector<double>& temps,
    const std::vector<double>& sample_log_freq_khz,
    const std::vector<double>& sample_temperature,
    const std::vector<double>& sample_log_esr,
    double temp_min,
    double temp_max,
    std::vector<std::vector<double>>& esr_grid,
    double& elapsed_s);

bool calculate_capacitor_esr_grids_gpu_batch(
    const std::vector<std::vector<std::pair<double, double>>>& batch_harmonics,
    const std::vector<double>& temps,
    const std::vector<double>& sample_log_freq_khz,
    const std::vector<double>& sample_temperature,
    const std::vector<double>& sample_log_esr,
    double temp_min,
    double temp_max,
    std::vector<std::vector<std::vector<double>>>& batch_esr_grids,
    double& elapsed_s);
