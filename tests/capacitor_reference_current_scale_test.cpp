#include "multi_physics_simulator/thermal_simulation/capacitor_loss_thermal_model.h"
#include "multi_physics_simulator/thermal_simulation/capacitor_reference_gpu.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Force this unit test down the CPU harmonic/ESR path without a CUDA runtime.
bool calculate_capacitor_harmonics_gpu(
    const std::vector<double>&,
    const std::vector<double>&,
    std::vector<std::pair<double, double>>& harmonics,
    double& elapsed_s) {
    harmonics.clear();
    elapsed_s = 0.0;
    return false;
}

bool calculate_capacitor_esr_grid_gpu(
    const std::vector<std::pair<double, double>>&,
    const std::vector<double>&,
    const std::vector<double>&,
    const std::vector<double>&,
    const std::vector<double>&,
    double,
    double,
    std::vector<std::vector<double>>& esr_grid,
    double& elapsed_s) {
    esr_grid.clear();
    elapsed_s = 0.0;
    return false;
}

bool calculate_capacitor_esr_grids_gpu_batch(
    const std::vector<std::vector<std::pair<double, double>>>&,
    const std::vector<double>&,
    const std::vector<double>&,
    const std::vector<double>&,
    const std::vector<double>&,
    double,
    double,
    std::vector<std::vector<std::vector<double>>>& esr_grids,
    double& elapsed_s) {
    esr_grids.clear();
    elapsed_s = 0.0;
    return false;
}

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    constexpr std::size_t sample_count = 513;
    constexpr double pi = 3.14159265358979323846;
    constexpr double duration_s = 1.0 / 60.0;
    std::vector<double> time(sample_count);
    std::vector<double> total_bank_current(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i) {
        const double fraction = static_cast<double>(i) /
                                static_cast<double>(sample_count - 1);
        time[i] = duration_s * fraction;
        total_bank_current[i] = 10.0 * std::sin(2.0 * pi * fraction);
    }

    const std::string esr_path =
        "Capacitor/cpp_standalone/data/esr_data.csv";
    const CapacitorReferenceThermalResult bank_result =
        calculate_capacitor_reference_thermal(
            time,
            total_bank_current,
            25.0,
            0.0,
            0.0,
            esr_path,
            1.0);
    const CapacitorReferenceThermalResult per_device_result =
        calculate_capacitor_reference_thermal(
            time,
            total_bank_current,
            25.0,
            0.0,
            0.0,
            esr_path,
            1.0 / 5.0);

    require(bank_result.valid && per_device_result.valid,
            "reference capacitor scale test requires valid CPU results");
    require(bank_result.loss > 0.0,
            "reference capacitor scale test requires non-zero loss");
    const double loss_ratio = per_device_result.loss / bank_result.loss;
    require(std::abs(loss_ratio - 1.0 / 25.0) < 1e-10,
            "1/5 harmonic-current scaling must produce 1/25 loss at fixed temperature");
    return 0;
}
