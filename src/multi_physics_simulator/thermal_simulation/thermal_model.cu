#include "thermal_model.h"

#include <cuda_runtime.h>
#include <cmath>

// Device function to calculate capacitor thermal response from capacitor losses
// hotspot_temp = internal_temp + power_loss * rth_amb
// surface_temp = internal_temp + power_loss * rth_surf
__device__ void calculate_capacitor_thermal_device(
    double capacitor_loss,
    double internal_temperature,
    double rth_amb,
    double rth_surf,
    double& capacitor_hotspot_temperature,
    double& capacitor_surface_temperature
) {
    capacitor_hotspot_temperature = internal_temperature + capacitor_loss * rth_amb;
    capacitor_surface_temperature = internal_temperature + capacitor_loss * rth_surf;
}

// Device function to calculate power module thermal response from ac_power
// Tj = T_amb + ac_power / 30000 * (100 - 45)
__device__ void calculate_power_module_thermal_device(
    double ac_power,
    double ambient_temperature,
    double& junction_temperature
) {
    // Thermal model: Tj = T_amb + ac_power / 30000 * (100 - 45)
    junction_temperature = ambient_temperature + (ac_power / 30000.0) * (100.0 - 45.0);
}

// CUDA kernel to calculate capacitor thermal response for multiple cases
__global__ void calculate_capacitor_thermal_kernel(
    const double* capacitor_losses,      // Input: Capacitor loss for each case
    const double* internal_temperatures, // Input: Internal temperature for each case
    const double* rth_amb_array,          // Input: Rth_amb for each case
    const double* rth_surf_array,         // Input: Rth_surf for each case
    int num_cases,                        // Input: Number of cases
    double* capacitor_hotspot_temps,     // Output: Capacitor hotspot temperature for each case
    double* capacitor_surface_temps       // Output: Capacitor surface temperature for each case
) {
    const int case_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (case_idx >= num_cases) {
        return;
    }
    
    calculate_capacitor_thermal_device(
        capacitor_losses[case_idx],
        internal_temperatures[case_idx],
        rth_amb_array[case_idx],
        rth_surf_array[case_idx],
        capacitor_hotspot_temps[case_idx],
        capacitor_surface_temps[case_idx]
    );
}

// CUDA kernel to calculate power module thermal response for multiple cases
__global__ void calculate_power_module_thermal_kernel(
    const double* ac_powers,             // Input: AC power for each case
    const double* ambient_temperatures,  // Input: Ambient temperature for each case
    int num_cases,                       // Input: Number of cases
    double* junction_temperatures        // Output: Junction temperature for each case
) {
    const int case_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (case_idx >= num_cases) {
        return;
    }
    
    calculate_power_module_thermal_device(
        ac_powers[case_idx],
        ambient_temperatures[case_idx],
        junction_temperatures[case_idx]
    );
}

// Host wrapper functions
CapacitorThermalResult calculate_capacitor_thermal(
    double capacitor_loss,
    double internal_temperature,
    double rth_amb,
    double rth_surf
) {
    CapacitorThermalResult result;
    result.capacitor_hotspot_temperature = internal_temperature + capacitor_loss * rth_amb;
    result.capacitor_surface_temperature = internal_temperature + capacitor_loss * rth_surf;
    return result;
}

// Host wrapper function
PowerModuleThermalResult calculate_power_module_thermal(
    double ac_power,
    double ambient_temperature
) {
    PowerModuleThermalResult result;
    result.junction_temperature = ambient_temperature + (ac_power / 30000.0) * (100.0 - 45.0);
    return result;
}
