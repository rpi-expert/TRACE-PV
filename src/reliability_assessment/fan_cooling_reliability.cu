#include "fan_cooling_reliability.h"
#include "reliability_models.h"

#include <cuda_runtime.h>
#include <cmath>

// Device function to calculate cooling fan lifetime using electrical model
// Lifetime_electrical = A * RH_op^n * exp(-E_a / (k_B * T_op))
__device__ double calculate_fan_lifetime_electrical_device(
    double temperature,
    double relative_humidity,
    const FanCoefficients& coeffs
) {
    // Convert temperature from Celsius to Kelvin
    double T_op_kelvin = temperature + ABSOLUTE_ZERO;
    
    // Safety checks
    if (coeffs.electrical.A <= 0.0 || T_op_kelvin <= 0.0 || relative_humidity <= 0.0) {
        return 0.0;
    }
    
    // Check for very small RH that might cause underflow
    if (relative_humidity < 1e-10) {
        return 0.0;
    }
    
    // Calculate lifetime components
    // RH_op^n
    double rh_factor = pow(relative_humidity, coeffs.electrical.n);
    
    // exp(-E_a / (k_B * T_op))
    double temp_factor = exp(coeffs.electrical.Ea / (BOLTZMANN_CONSTANT * T_op_kelvin));
    
    // Calculate lifetime (hours)
    double lifetime = coeffs.electrical.A * rh_factor * temp_factor;
    
    return lifetime;
}

// Device function to calculate cooling fan lifetime using mechanical model
// Lifetime_mechanical = A * exp(c * T_op)
__device__ double calculate_fan_lifetime_mechanical_device(
    double temperature,
    const FanCoefficients& coeffs
) {
    // T_op is in Celsius (as per the formula)
    // Safety checks
    if (coeffs.mechanical.A <= 0.0) {
        return 0.0;
    }
    
    // Calculate lifetime (hours)
    // Lifetime_mechanical = A * exp(c * T_op)
    double lifetime = coeffs.mechanical.A * exp(coeffs.mechanical.c * temperature);
    
    return lifetime;
}

// CUDA kernel to calculate fan lifetime (electrical) for multiple cases
__global__ void calculate_fan_lifetime_electrical_kernel(
    const double* temperatures,
    const double* relative_humidities,
    const FanCoefficients* coeffs,
    int num_cases,
    double* lifetimes
) {
    const int case_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (case_idx >= num_cases) {
        return;
    }
    
    lifetimes[case_idx] = calculate_fan_lifetime_electrical_device(
        temperatures[case_idx],
        relative_humidities[case_idx],
        coeffs[case_idx]
    );
}

// CUDA kernel to calculate fan lifetime (mechanical) for multiple cases
__global__ void calculate_fan_lifetime_mechanical_kernel(
    const double* temperatures,
    const FanCoefficients* coeffs,
    int num_cases,
    double* lifetimes
) {
    const int case_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (case_idx >= num_cases) {
        return;
    }
    
    lifetimes[case_idx] = calculate_fan_lifetime_mechanical_device(
        temperatures[case_idx],
        coeffs[case_idx]
    );
}

// Host wrapper functions (for backward compatibility)
double calculate_fan_lifetime_electrical(
    double temperature,
    double relative_humidity,
    const FanCoefficients& coeffs
) {
    // Allocate device memory
    double* d_temperature;
    double* d_rh;
    FanCoefficients* d_coeffs;
    double* d_lifetime;
    
    cudaMalloc(&d_temperature, sizeof(double));
    cudaMalloc(&d_rh, sizeof(double));
    cudaMalloc(&d_coeffs, sizeof(FanCoefficients));
    cudaMalloc(&d_lifetime, sizeof(double));
    
    cudaMemcpy(d_temperature, &temperature, sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_rh, &relative_humidity, sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_coeffs, &coeffs, sizeof(FanCoefficients), cudaMemcpyHostToDevice);
    
    // Launch kernel
    calculate_fan_lifetime_electrical_kernel<<<1, 1>>>(
        d_temperature, d_rh, d_coeffs, 1, d_lifetime
    );
    
    // Copy result back
    double lifetime;
    cudaMemcpy(&lifetime, d_lifetime, sizeof(double), cudaMemcpyDeviceToHost);
    
    // Cleanup
    cudaFree(d_temperature);
    cudaFree(d_rh);
    cudaFree(d_coeffs);
    cudaFree(d_lifetime);
    
    return lifetime;
}

double calculate_fan_lifetime_mechanical(
    double temperature,
    const FanCoefficients& coeffs
) {
    // Allocate device memory
    double* d_temperature;
    FanCoefficients* d_coeffs;
    double* d_lifetime;
    
    cudaMalloc(&d_temperature, sizeof(double));
    cudaMalloc(&d_coeffs, sizeof(FanCoefficients));
    cudaMalloc(&d_lifetime, sizeof(double));
    
    cudaMemcpy(d_temperature, &temperature, sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_coeffs, &coeffs, sizeof(FanCoefficients), cudaMemcpyHostToDevice);
    
    // Launch kernel
    calculate_fan_lifetime_mechanical_kernel<<<1, 1>>>(
        d_temperature, d_coeffs, 1, d_lifetime
    );
    
    // Copy result back
    double lifetime;
    cudaMemcpy(&lifetime, d_lifetime, sizeof(double), cudaMemcpyDeviceToHost);
    
    // Cleanup
    cudaFree(d_temperature);
    cudaFree(d_coeffs);
    cudaFree(d_lifetime);
    
    return lifetime;
}

