#ifndef PCB_RELIABILITY_H
#define PCB_RELIABILITY_H

#include "reliability_models.h"
#include <vector>

/**
 * Calculate PCB cycles to failure (Nf) for a given delta_T using strain energy model
 * Based on the Python PCB_lifetime function
 * 
 * @param delta_T Temperature range (delta T) in Kelvin or Celsius (absolute value used)
 * @param pcb_params PCB parameters structure
 * @return Number of cycles to failure (N_f50_SE)
 */
double calculate_pcb_nf(double delta_T, const PCBParameters& pcb_params);

/**
 * Calculate PCB stressor from rainflow counting results
 * Uses rainflow counting on internal temperature, then calculates Nf for each deltaT
 * Accumulates cycles/Nf as stressor (fraction of lifetime consumed by cycles)
 * 
 * @param delta_range Temperature ranges from rainflow counting
 * @param delta_cycle Number of cycles for each range
 * @param pcb_params PCB parameters structure
 * @return Accumulated stressor (sum of cycles/Nf for each cycle type)
 */
double calculate_pcb_stressor(
    const std::vector<double>& delta_range,
    const std::vector<double>& delta_cycle,
    const PCBParameters& pcb_params
);

#endif // PCB_RELIABILITY_H

