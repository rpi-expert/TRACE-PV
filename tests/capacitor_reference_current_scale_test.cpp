#include "multi_physics_simulator/thermal_simulation/capacitor_loss_thermal_model.h"
#include "multi_physics_simulator/thermal_simulation/capacitor_reference_gpu.h"

#include <cmath>
#include <filesystem>
#include <fstream>
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

    const std::filesystem::path esr_file =
        std::filesystem::temp_directory_path() /
        "tracepv-capacitor-reference-current-scale-esr.csv";
    {
        std::ofstream out(esr_file);
        require(static_cast<bool>(out), "failed to create the test ESR table");
        for (const double temperature : {-25.0, 25.0, 85.0}) {
            out << "0.06,0.10," << temperature << '\n'
                << "1.0,0.08," << temperature << '\n'
                << "10.0,0.05," << temperature << '\n'
                << "100.0,0.03," << temperature << '\n';
        }
        require(static_cast<bool>(out), "failed to write the test ESR table");
    }
    const std::string esr_path = esr_file.string();
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
    std::error_code cleanup_error;
    std::filesystem::remove(esr_file, cleanup_error);
    return 0;
}
