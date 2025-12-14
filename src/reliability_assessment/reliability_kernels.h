#ifndef RELIABILITY_KERNELS_H
#define RELIABILITY_KERNELS_H

#include "reliability_models.h"

/**
 * Calculate fan cooling reliability stressors for a batch of cases on GPU
 * Returns 4 stressors: internal electrical, internal mechanical, ambient electrical, ambient mechanical
 * 
 * @param ambient_temps Array of ambient temperatures (Celsius)
 * @param ambient_rhs Array of ambient relative humidities (%)
 * @param internal_temps Array of internal temperatures (Celsius)
 * @param internal_rhs Array of internal relative humidities (%)
 * @param num_cases Number of cases in the batch
 * @param coeffs Fan coefficients
 * @param stressors Output array of 4 stressors (internal_electrical, internal_mechanical, ambient_electrical, ambient_mechanical)
 */
void calculate_fan_reliability_stressors_gpu(
    const double* ambient_temps,
    const double* ambient_rhs,
    const double* internal_temps,
    const double* internal_rhs,
    int num_cases,
    const FanCoefficients& coeffs,
    double* stressors
);

/**
 * Calculate capacitor reliability stressor for a batch of cases on GPU
 * 
 * @param voltages Array of capacitor voltages (V)
 * @param hotspot_temps Array of capacitor hotspot temperatures (Celsius)
 * @param internal_rhs Array of internal relative humidities (%)
 * @param num_cases Number of cases in the batch
 * @param capacitor_type Capacitor type
 * @param coeffs Capacitor coefficients
 * @param stressor Output total stressor (sum of 5 * (1 / (lifetime * 60)) for all cases, converting hours to minutes and calculating for 5 minutes)
 */
void calculate_capacitor_reliability_stressor_gpu(
    const double* voltages,
    const double* hotspot_temps,
    const double* internal_rhs,
    int num_cases,
    CapacitorType capacitor_type,
    const CapacitorCoefficients& coeffs,
    double& stressor
);

#endif // RELIABILITY_KERNELS_H

