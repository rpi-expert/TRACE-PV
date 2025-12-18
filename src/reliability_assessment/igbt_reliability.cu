#include "igbt_reliability.h"
#include "reliability_models.h"

#include <cuda_runtime.h>
#include <cmath>
#include <algorithm>

// Device function to calculate IGBT (power module) lifetime based on reliability model
// Note: This is a simplified version for device function. Full rainflow counting
// should be done on host or in a separate kernel.
// Uses deltaT model: Nf = A * ΔT^(n) * e^(Ea / (K_B * T_max))
__device__ double calculate_igbt_lifetime_device(
    double delta_range,
    double delta_cycle,
    double Tj_max,
    const PowerModuleCoefficients& coeffs
) {
    // Convert Tj_max from Celsius to Kelvin
    double Tj_max_kelvin = Tj_max + ABSOLUTE_ZERO;
    
    // Calculate number of cycles to failure using deltaT model
    // Nf = A * ΔT^(n) * e^(Ea / (K_B * T_max))
    double exp_factor = exp(coeffs.deltaT_model.Ea / (BOLTZMANN_CONSTANT * Tj_max_kelvin));
    double Nf = coeffs.deltaT_model.A * pow(delta_range, coeffs.deltaT_model.n) * exp_factor;
    
    if (Nf > 0.0) {
        // Damage = actual cycles / cycles to failure
        double damage = delta_cycle / Nf;
        // Lifetime is inversely related to damage
        double lifetime = 1.0 / damage;
        return lifetime;
    }
    
    return 1e10; // Very long lifetime if no damage
}

// CUDA kernel to calculate IGBT lifetime for multiple temperature cycles
// This kernel processes individual cycles. For full rainflow counting,
// a separate kernel should be used to process the full temperature history.
__global__ void calculate_igbt_lifetime_cycle_kernel(
    const double* delta_ranges,
    const double* delta_cycles,
    const double* Tj_maxs,
    const PowerModuleCoefficients* coeffs,
    int num_cycles,
    double* lifetimes
) {
    const int cycle_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (cycle_idx >= num_cycles) {
        return;
    }
    
    lifetimes[cycle_idx] = calculate_igbt_lifetime_device(
        delta_ranges[cycle_idx],
        delta_cycles[cycle_idx],
        Tj_maxs[cycle_idx],
        coeffs[cycle_idx]
    );
}

// CUDA kernel to calculate cumulative IGBT lifetime from multiple cycles
// This implements Miner's rule for cumulative damage
__global__ void calculate_igbt_lifetime_cumulative_kernel(
    const double* delta_ranges,
    const double* delta_cycles,
    double Tj_max,
    const PowerModuleCoefficients* coeffs,
    int num_cycles,
    double* cumulative_lifetime
) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx != 0) {
        return; // Only first thread computes cumulative
    }
    
    // Convert Tj_max from Celsius to Kelvin
    double Tj_max_kelvin = Tj_max + ABSOLUTE_ZERO;
    
    // Calculate cumulative damage using Miner's rule
    // Uses deltaT model: Nf = A * ΔT^(n) * e^(Ea / (K_B * T_max))
    double cumulative_damage = 0.0;
    double exp_factor = exp(coeffs[0].deltaT_model.Ea / (BOLTZMANN_CONSTANT * Tj_max_kelvin));
    
    for (int i = 0; i < num_cycles; ++i) {
        double range = delta_ranges[i];
        double cycles = delta_cycles[i];
        
        // Calculate number of cycles to failure for this range using deltaT model
        double Nf = coeffs[0].deltaT_model.A * pow(range, coeffs[0].deltaT_model.n) * exp_factor;
        
        if (Nf > 0.0) {
            // Damage = actual cycles / cycles to failure
            cumulative_damage += cycles / Nf;
        }
    }
    
    // Lifetime calculation
    if (cumulative_damage <= 0.0) {
        cumulative_lifetime[0] = 1e10; // Very long lifetime
    } else {
        // Lifetime is inversely related to cumulative damage
        double lifetime = 1.0 / cumulative_damage;
        cumulative_lifetime[0] = lifetime;
    }
}

// Host wrapper function (for backward compatibility)
double calculate_igbt_lifetime(
    const std::vector<double>& delta_range,
    const std::vector<double>& delta_cycle,
    double Tj_max,
    const PowerModuleCoefficients& coeffs
) {
    if (delta_range.empty() || delta_range.size() != delta_cycle.size()) {
        return 0.0;
    }
    
    const int num_cycles = static_cast<int>(delta_range.size());
    
    // Allocate device memory
    double* d_delta_range;
    double* d_delta_cycle;
    double* d_Tj_max;
    PowerModuleCoefficients* d_coeffs;
    double* d_lifetime;
    
    cudaMalloc(&d_delta_range, num_cycles * sizeof(double));
    cudaMalloc(&d_delta_cycle, num_cycles * sizeof(double));
    cudaMalloc(&d_Tj_max, sizeof(double));
    cudaMalloc(&d_coeffs, sizeof(PowerModuleCoefficients));
    cudaMalloc(&d_lifetime, sizeof(double));
    
    cudaMemcpy(d_delta_range, delta_range.data(), num_cycles * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_delta_cycle, delta_cycle.data(), num_cycles * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_Tj_max, &Tj_max, sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_coeffs, &coeffs, sizeof(PowerModuleCoefficients), cudaMemcpyHostToDevice);
    
    // Launch kernel for cumulative calculation
    calculate_igbt_lifetime_cumulative_kernel<<<1, 1>>>(
        d_delta_range, d_delta_cycle, Tj_max, d_coeffs, num_cycles, d_lifetime
    );
    
    // Copy result back
    double lifetime;
    cudaMemcpy(&lifetime, d_lifetime, sizeof(double), cudaMemcpyDeviceToHost);
    
    // Cleanup
    cudaFree(d_delta_range);
    cudaFree(d_delta_cycle);
    cudaFree(d_Tj_max);
    cudaFree(d_coeffs);
    cudaFree(d_lifetime);
    
    return lifetime;
}

// Calculate Nf (cycles to failure) for IGBT using deltaT model
// Nf = A * ΔT^(n) * e^(Ea / (K_B * T_max))
// Input: deltaT (temperature range), Tj_max (maximum junction temperature), coefficients
// Output: Nf (number of cycles to failure)
double calculate_igbt_nf_deltaT(double deltaT, double Tj_max, const PowerModuleCoefficients& coeffs) {
    // Convert Tj_max from Celsius to Kelvin
    double Tj_max_kelvin = Tj_max + ABSOLUTE_ZERO;
    
    // Calculate Nf using deltaT model: Nf = A * ΔT^(n) * e^(Ea / (K_B * T_max))
    double exp_factor = std::exp(coeffs.deltaT_model.Ea / (BOLTZMANN_CONSTANT * Tj_max_kelvin));
    double Nf = coeffs.deltaT_model.A * std::pow(deltaT, coeffs.deltaT_model.n) * exp_factor;
    
    return (Nf > 0.0) ? Nf : 1e10;  // Return large value if invalid
}

// Calculate lifetime for IGBT using Arrhenius model (corrosion/dendrites model)
// Acceleration Factor: AF = (rh/RH_ref)^n1 * exp(Ea / kB * (1/T_ref - 1/T_internal)) * (V_ref/voltage)^n2
// Lifetime = reference_lifetime / AF
// Input: T_internal (internal temperature in Celsius from internal conditions calculation), 
//        rh (relative humidity in % from mission profile), 
//        voltage (AC voltage in V from mission profile), 
//        coefficients (contains RH_ref, T_ref, V_ref from IGBT profile)
// Output: Lifetime in hours
double calculate_igbt_lifetime_arrhenius(
    double T_internal, 
    double rh, 
    double voltage, 
    const PowerModuleCoefficients& coeffs
) {
    // Convert T_internal from Celsius to Kelvin
    double T_internal_kelvin = T_internal + ABSOLUTE_ZERO;
    
    // Get reference values from IGBT profile coefficients
    double T_ref_kelvin = coeffs.arrhenius_model.T_ref;  // Already in Kelvin from JSON
    double rh_ref = coeffs.arrhenius_model.RH_ref;  // Reference RH in %
    double voltage_ref = coeffs.arrhenius_model.V_ref;  // Reference voltage in V
    
    // Calculate acceleration factor using corrosion/dendrites model
    // AF = (rh/RH_ref)^n1 * exp(Ea / kB * (1/T_ref - 1/Tj)) * (V_ref/voltage)^n2
    double rh_ratio = rh / rh_ref ;
    double rh_factor = std::pow(rh_ratio, coeffs.arrhenius_model.n1);
    
    double temp_factor = std::exp(coeffs.arrhenius_model.Ea / BOLTZMANN_CONSTANT * 
                                   (1.0 / T_ref_kelvin - 1.0 / T_internal_kelvin));
    
    double voltage_factor = std::pow(voltage_ref / voltage, coeffs.arrhenius_model.n2);
    
    double AF = rh_factor * temp_factor * voltage_factor;
    
    // Lifetime = reference_lifetime / AF
    // Use A as reference lifetime (in hours)
    double lifetime = coeffs.arrhenius_model.A / AF;
    
    return (lifetime > 0.0) ? lifetime : 1e10;  // Return large value if invalid
}

