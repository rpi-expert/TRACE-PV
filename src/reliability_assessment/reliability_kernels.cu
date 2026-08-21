#include "reliability_kernels.h"
#include "reliability_models.h"
#include <cuda_runtime.h>
#include <cmath>

// Forward declarations of device functions (they're in other .cu files)
// For CUDA separate compilation, use extern __device__ to reference device functions from other files
extern __device__ double calculate_fan_lifetime_electrical_device(
    double temperature,
    double relative_humidity,
    const FanCoefficients& coeffs
);

extern __device__ double calculate_fan_lifetime_mechanical_device(
    double temperature,
    const FanCoefficients& coeffs
);

extern __device__ double calculate_capacitor_lifetime_device(
    double dc_voltage,
    double hotspot_temp,
    double internal_rh,
    CapacitorType capacitor_type,
    const CapacitorCoefficients& coeffs
);

// CUDA kernel to calculate fan reliability stressors for a batch
__global__ void calculate_fan_stressors_kernel(
    const double* ambient_temps,
    const double* ambient_rhs,
    const double* internal_temps,
    const double* internal_rhs,
    int num_cases,
    FanCoefficients coeffs,
    double* stressors  // [0]=internal_electrical, [1]=internal_mechanical, [2]=ambient_electrical, [3]=ambient_mechanical
) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_cases) return;
    
    // Calculate lifetimes for this case
    double internal_electrical_lifetime = calculate_fan_lifetime_electrical_device(
        internal_temps[idx],
        internal_rhs[idx],
        coeffs
    );
    
    double internal_mechanical_lifetime = calculate_fan_lifetime_mechanical_device(
        internal_temps[idx],
        coeffs
    );
    
    double ambient_electrical_lifetime = calculate_fan_lifetime_electrical_device(
        ambient_temps[idx],
        ambient_rhs[idx],
        coeffs
    );
    
    double ambient_mechanical_lifetime = calculate_fan_lifetime_mechanical_device(
        ambient_temps[idx],
        coeffs
    );
    
    
    // Convert to stressors: 5 * (1 / (lifetime * 60)) - convert from hours to minutes and calculate for 5 minutes
    if (internal_electrical_lifetime > 0.0 && !isinf(internal_electrical_lifetime) && !isnan(internal_electrical_lifetime)) {
        atomicAdd(&stressors[0], 5.0 * (1.0 / (internal_electrical_lifetime * 60.0)));
    }
    if (internal_mechanical_lifetime > 0.0 && !isinf(internal_mechanical_lifetime) && !isnan(internal_mechanical_lifetime)) {
        atomicAdd(&stressors[1], 5.0 * (1.0 / (internal_mechanical_lifetime * 60.0)));
    }
    if (ambient_electrical_lifetime > 0.0 && !isinf(ambient_electrical_lifetime) && !isnan(ambient_electrical_lifetime)) {
        atomicAdd(&stressors[2], 5.0 * (1.0 / (ambient_electrical_lifetime * 60.0)));
    }
    if (ambient_mechanical_lifetime > 0.0 && !isinf(ambient_mechanical_lifetime) && !isnan(ambient_mechanical_lifetime)) {
        atomicAdd(&stressors[3], 5.0 * (1.0 / (ambient_mechanical_lifetime * 60.0)));
    }
}

// CUDA kernel to calculate capacitor reliability stressor for a batch
__global__ void calculate_capacitor_stressor_kernel(
    const double* voltages,
    const double* hotspot_temps,
    const double* internal_rhs,
    int num_cases,
    CapacitorType capacitor_type,
    CapacitorCoefficients coeffs,
    double* stressor
) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_cases) return;
    
    double lifetime = calculate_capacitor_lifetime_device(
        voltages[idx],
        hotspot_temps[idx],
        internal_rhs[idx],
        capacitor_type,
        coeffs
    );
    
    // Convert to stressor: 5 * (1 / (lifetime * 60)) - convert from hours to minutes and calculate for 5 minutes
    if (lifetime > 0.0 && !isinf(lifetime) && !isnan(lifetime)) {
        atomicAdd(stressor, 5.0 * (1.0 / (lifetime * 60.0)));
    }
}

// Host wrapper for fan reliability stressors
void calculate_fan_reliability_stressors_gpu(
    const double* ambient_temps,
    const double* ambient_rhs,
    const double* internal_temps,
    const double* internal_rhs,
    int num_cases,
    const FanCoefficients& coeffs,
    double* stressors
) {
    // Allocate device memory
    double* d_ambient_temps;
    double* d_ambient_rhs;
    double* d_internal_temps;
    double* d_internal_rhs;
    double* d_stressors;
    
    cudaMalloc(&d_ambient_temps, num_cases * sizeof(double));
    cudaMalloc(&d_ambient_rhs, num_cases * sizeof(double));
    cudaMalloc(&d_internal_temps, num_cases * sizeof(double));
    cudaMalloc(&d_internal_rhs, num_cases * sizeof(double));
    cudaMalloc(&d_stressors, 4 * sizeof(double));
    
    // Copy data to device
    cudaMemcpy(d_ambient_temps, ambient_temps, num_cases * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_ambient_rhs, ambient_rhs, num_cases * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_internal_temps, internal_temps, num_cases * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_internal_rhs, internal_rhs, num_cases * sizeof(double), cudaMemcpyHostToDevice);
    
    // Initialize stressors to zero
    double zero_stressors[4] = {0.0, 0.0, 0.0, 0.0};
    cudaMemcpy(d_stressors, zero_stressors, 4 * sizeof(double), cudaMemcpyHostToDevice);
    
    // Launch kernel
    int threads_per_block = 256;
    int num_blocks = (num_cases + threads_per_block - 1) / threads_per_block;
    calculate_fan_stressors_kernel<<<num_blocks, threads_per_block>>>(
        d_ambient_temps, d_ambient_rhs, d_internal_temps, d_internal_rhs,
        num_cases, coeffs, d_stressors
    );
    
    // Copy results back
    cudaMemcpy(stressors, d_stressors, 4 * sizeof(double), cudaMemcpyDeviceToHost);
    
    // Cleanup
    cudaFree(d_ambient_temps);
    cudaFree(d_ambient_rhs);
    cudaFree(d_internal_temps);
    cudaFree(d_internal_rhs);
    cudaFree(d_stressors);
}

// Host wrapper for capacitor reliability stressor
void calculate_capacitor_reliability_stressor_gpu(
    const double* voltages,
    const double* hotspot_temps,
    const double* internal_rhs,
    int num_cases,
    CapacitorType capacitor_type,
    const CapacitorCoefficients& coeffs,
    double& stressor
) {
    // Allocate device memory
    double* d_voltages;
    double* d_hotspot_temps;
    double* d_internal_rhs;
    double* d_stressor;
    
    cudaMalloc(&d_voltages, num_cases * sizeof(double));
    cudaMalloc(&d_hotspot_temps, num_cases * sizeof(double));
    cudaMalloc(&d_internal_rhs, num_cases * sizeof(double));
    cudaMalloc(&d_stressor, sizeof(double));
    
    // Copy data to device
    cudaMemcpy(d_voltages, voltages, num_cases * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_hotspot_temps, hotspot_temps, num_cases * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_internal_rhs, internal_rhs, num_cases * sizeof(double), cudaMemcpyHostToDevice);
    
    // Initialize stressor to zero
    double zero = 0.0;
    cudaMemcpy(d_stressor, &zero, sizeof(double), cudaMemcpyHostToDevice);
    
    // Launch kernel
    int threads_per_block = 256;
    int num_blocks = (num_cases + threads_per_block - 1) / threads_per_block;
    calculate_capacitor_stressor_kernel<<<num_blocks, threads_per_block>>>(
        d_voltages, d_hotspot_temps, d_internal_rhs,
        num_cases, capacitor_type, coeffs, d_stressor
    );
    
    // Copy result back
    cudaMemcpy(&stressor, d_stressor, sizeof(double), cudaMemcpyDeviceToHost);
    
    // Cleanup
    cudaFree(d_voltages);
    cudaFree(d_hotspot_temps);
    cudaFree(d_internal_rhs);
    cudaFree(d_stressor);
}

