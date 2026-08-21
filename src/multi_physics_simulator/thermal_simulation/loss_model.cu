#include "loss_model.h"

#include <cuda_runtime.h>
#include <cmath>
#include <stdexcept>

// Device function to calculate capacitor losses from RMS current and ESR
// Power loss = I_rms^2 * ESR
// Note: ESR should already account for topology (doubled for 3-level topology where two capacitors are in series)
// For 3-level: ESR_total = ESR1 + ESR2 = 2 * ESR (if capacitors are identical)
__device__ double calculate_capacitor_loss_device(double I_cap_rms, double esr) {
    if (I_cap_rms < 0.0 || esr < 0.0) {
        return 0.0;
    }
    return I_cap_rms * I_cap_rms * esr;
}

// Device-side legacy entry point. Exact power-module loss is calculated by the
// reference IGBT loss/thermal model, which has access to device states,
// switching events, parameter tables, and temperature feedback.
__device__ double calculate_power_module_loss_device(
    const double* V_ce,
    const double* I_c,
    int num_samples
) {
    return NAN;
}

// CUDA kernel to calculate capacitor losses for multiple cases
__global__ void calculate_capacitor_loss_kernel(
    const double* I_cap_rms_array,   // Input: RMS capacitor current for each case
    const double* esr_array,          // Input: ESR for each case
    int num_cases,                    // Input: Number of cases
    double* capacitor_losses          // Output: Capacitor loss for each case
) {
    const int case_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (case_idx >= num_cases) {
        return;
    }
    
    capacitor_losses[case_idx] = calculate_capacitor_loss_device(
        I_cap_rms_array[case_idx],
        esr_array[case_idx]
    );
}

// CUDA kernel to calculate power module losses for multiple cases
__global__ void calculate_power_module_loss_kernel(
    const double* V_ce_array,       // Input: V_ce for all samples (flattened)
    const double* I_c_array,         // Input: I_c for all samples (flattened)
    int* sample_counts,              // Input: Number of samples per case
    int num_cases,                   // Input: Number of cases
    double* power_module_losses      // Output: Power module loss for each case
) {
    const int case_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (case_idx >= num_cases) {
        return;
    }
    
    // Calculate offset for this case
    int offset = 0;
    for (int i = 0; i < case_idx; ++i) {
        offset += sample_counts[i];
    }
    
    const int num_samples = sample_counts[case_idx];
    power_module_losses[case_idx] = calculate_power_module_loss_device(
        V_ce_array + offset,
        I_c_array + offset,
        num_samples
    );
}

// Host wrapper functions
CapacitorLossResult calculate_capacitor_loss(double I_cap_rms, double esr) {
    CapacitorLossResult result;
    result.capacitor_loss = (I_cap_rms < 0.0 || esr < 0.0) ? 0.0 : I_cap_rms * I_cap_rms * esr;
    return result;
}

// Host wrapper function
PowerModuleLossResult calculate_power_module_loss(
    const std::vector<double>& V_ce,
    const std::vector<double>& I_c
) {
    PowerModuleLossResult result;
    result.valid = false;
    result.message =
        "Exact IGBT loss cannot be calculated from V_ce/I_c alone. Use "
        "calculate_igbt_reference_thermal_batch() and its average_total_loss.";
    return result;
}
