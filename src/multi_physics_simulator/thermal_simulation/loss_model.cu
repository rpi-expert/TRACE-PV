#include "loss_model.h"

#include <cuda_runtime.h>
#include <cmath>

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

// Device function to calculate power module losses from voltage and current stress
// This is a placeholder - actual implementation should use switching and conduction losses
__device__ double calculate_power_module_loss_device(
    const double* V_ce,
    const double* I_c,
    int num_samples
) {
    // TODO: Implement actual power module loss calculation model
    // For now, return zero loss
    // Actual model would calculate:
    // - Conduction losses: P_cond = V_ce_sat * I_c (when on)
    // - Switching losses: P_sw = f_sw * (E_on + E_off) based on V_ce and I_c
    return 0.0;
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
    
    // Allocate device memory
    double* d_I_cap_rms;
    double* d_esr;
    double* d_loss;
    
    cudaMalloc(&d_I_cap_rms, sizeof(double));
    cudaMalloc(&d_esr, sizeof(double));
    cudaMalloc(&d_loss, sizeof(double));
    
    cudaMemcpy(d_I_cap_rms, &I_cap_rms, sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_esr, &esr, sizeof(double), cudaMemcpyHostToDevice);
    
    // Launch kernel
    calculate_capacitor_loss_kernel<<<1, 1>>>(d_I_cap_rms, d_esr, 1, d_loss);
    
    // Copy result back
    double h_loss;
    cudaMemcpy(&h_loss, d_loss, sizeof(double), cudaMemcpyDeviceToHost);
    result.capacitor_loss = h_loss;
    
    // Cleanup
    cudaFree(d_I_cap_rms);
    cudaFree(d_esr);
    cudaFree(d_loss);
    
    return result;
}

// Host wrapper function
PowerModuleLossResult calculate_power_module_loss(
    const std::vector<double>& V_ce,
    const std::vector<double>& I_c
) {
    PowerModuleLossResult result;
    
    if (V_ce.empty() || I_c.empty() || V_ce.size() != I_c.size()) {
        result.power_module_loss = 0.0;
        return result;
    }
    
    // Allocate device memory
    double* d_V_ce;
    double* d_I_c;
    cudaMalloc(&d_V_ce, V_ce.size() * sizeof(double));
    cudaMalloc(&d_I_c, I_c.size() * sizeof(double));
    cudaMemcpy(d_V_ce, V_ce.data(), V_ce.size() * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_I_c, I_c.data(), I_c.size() * sizeof(double), cudaMemcpyHostToDevice);
    
    // Allocate output
    double* d_loss;
    cudaMalloc(&d_loss, sizeof(double));
    
    // Allocate sample counts
    int* d_sample_counts;
    cudaMalloc(&d_sample_counts, sizeof(int));
    int h_sample_count = static_cast<int>(V_ce.size());
    cudaMemcpy(d_sample_counts, &h_sample_count, sizeof(int), cudaMemcpyHostToDevice);
    
    // Launch kernel
    calculate_power_module_loss_kernel<<<1, 1>>>(d_V_ce, d_I_c, d_sample_counts, 1, d_loss);
    
    // Copy result back
    double h_loss;
    cudaMemcpy(&h_loss, d_loss, sizeof(double), cudaMemcpyDeviceToHost);
    result.power_module_loss = h_loss;
    
    // Cleanup
    cudaFree(d_V_ce);
    cudaFree(d_I_c);
    cudaFree(d_loss);
    cudaFree(d_sample_counts);
    
    return result;
}

