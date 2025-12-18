#include "capacitor_reliability.h"
#include "reliability_models.h"

#include <cuda_runtime.h>
#include <cmath>

// Device function to calculate capacitor lifetime based on reliability model
__device__ double calculate_capacitor_lifetime_device(
    double dc_voltage,
    double hotspot_temp,
    double internal_rh,
    CapacitorType capacitor_type,
    const CapacitorCoefficients& coeffs
) {
    double lifetime = 0.0;
    
    if (capacitor_type == CapacitorType::FILM) {
        // Film Capacitor Failure Model:
        // Lifetime = A * (RH_op)^-n * exp((E_a / k_B) * (1 / T_op)) * exp(β * V_op)
        const FilmCapacitorCoefficients& film = coeffs.coeffs.film;
        
        // Convert temperature from Celsius to Kelvin
        double T_op_kelvin = hotspot_temp + ABSOLUTE_ZERO;
        
        // Safety checks
        if (film.A <= 0.0 || T_op_kelvin <= 0.0 || internal_rh <= 0.0) {
            return 0.0;
        }
        
        // Calculate lifetime components
        double rh_factor = pow(internal_rh, -film.n);
        double temp_factor = exp((film.Ea / BOLTZMANN_CONSTANT) * (1.0 / T_op_kelvin));
        double voltage_factor = exp(film.beta * dc_voltage);
        
        // Calculate lifetime (hours)
        lifetime = film.A * rh_factor * temp_factor * voltage_factor;
        
    } else if (capacitor_type == CapacitorType::ALUMINUM_ELECTROLYTIC) {
        // Aluminum Electrolytic Capacitor Failure Model:
        // Lifetime = L_0 * (V / V_0)^-β_1 * 2^((T_0 - T) / 10)
        const AluminumElectrolyticCoefficients& aluminum = coeffs.coeffs.aluminum;
        
        // Safety checks
        if (aluminum.L0 <= 0.0 || aluminum.V0 <= 0.0) {
            return 0.0;
        }
        
        // Use beta_min as default (or could use average: (beta_min + beta_max) / 2.0)
        double beta_1 = aluminum.beta_min;
        
        // Calculate voltage factor: (V / V_0)^-β_1
        // Note: dc_voltage should already account for topology (divided by 2 for 3-level topology)
        // For 3-level: voltage_ratio = (vdc_target / 2) / V0
        // For 2-level: voltage_ratio = vdc_target / V0
        if (dc_voltage <= 0.0) {
            return 0.0;
        }
        double voltage_ratio = dc_voltage / aluminum.V0;
        double voltage_factor = pow(voltage_ratio, -beta_1);
        
        // Calculate temperature factor: 2^((T_0 - T) / 10)
        // T_0 is in Celsius, hotspot_temp is in Celsius
        double temp_factor = pow(2.0, (aluminum.T0 - hotspot_temp) / 10.0);
        
        // Calculate lifetime (hours)
        lifetime = aluminum.L0 * voltage_factor * temp_factor;
        
    } else {
        // Unknown capacitor type, return 0
        lifetime = 0.0;
    }
    
    return lifetime;
}

// CUDA kernel to calculate capacitor lifetime for multiple cases
__global__ void calculate_capacitor_lifetime_kernel(
    const double* dc_voltages,
    const double* hotspot_temps,
    const double* internal_rhs,
    const CapacitorType* capacitor_types,
    const CapacitorCoefficients* coeffs,
    int num_cases,
    double* lifetimes
) {
    const int case_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (case_idx >= num_cases) {
        return;
    }
    
    lifetimes[case_idx] = calculate_capacitor_lifetime_device(
        dc_voltages[case_idx],
        hotspot_temps[case_idx],
        internal_rhs[case_idx],
        capacitor_types[case_idx],
        coeffs[case_idx]
    );
}

// Host wrapper function (for backward compatibility)
double calculate_capacitor_lifetime(
    double dc_voltage,
    double hotspot_temp,
    double internal_rh,
    CapacitorType capacitor_type,
    const CapacitorCoefficients& coeffs
) {
    // Allocate device memory
    double* d_dc_voltage;
    double* d_hotspot_temp;
    double* d_rh;
    CapacitorType* d_type;
    CapacitorCoefficients* d_coeffs;
    double* d_lifetime;
    
    cudaMalloc(&d_dc_voltage, sizeof(double));
    cudaMalloc(&d_hotspot_temp, sizeof(double));
    cudaMalloc(&d_rh, sizeof(double));
    cudaMalloc(&d_type, sizeof(CapacitorType));
    cudaMalloc(&d_coeffs, sizeof(CapacitorCoefficients));
    cudaMalloc(&d_lifetime, sizeof(double));
    
    cudaMemcpy(d_dc_voltage, &dc_voltage, sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_hotspot_temp, &hotspot_temp, sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_rh, &internal_rh, sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_type, &capacitor_type, sizeof(CapacitorType), cudaMemcpyHostToDevice);
    cudaMemcpy(d_coeffs, &coeffs, sizeof(CapacitorCoefficients), cudaMemcpyHostToDevice);
    
    // Launch kernel
    calculate_capacitor_lifetime_kernel<<<1, 1>>>(
        d_dc_voltage, d_hotspot_temp, d_rh, d_type, d_coeffs, 1, d_lifetime
    );
    
    // Copy result back
    double lifetime;
    cudaMemcpy(&lifetime, d_lifetime, sizeof(double), cudaMemcpyDeviceToHost);
    
    // Cleanup
    cudaFree(d_dc_voltage);
    cudaFree(d_hotspot_temp);
    cudaFree(d_rh);
    cudaFree(d_type);
    cudaFree(d_coeffs);
    cudaFree(d_lifetime);
    
    return lifetime;
}

