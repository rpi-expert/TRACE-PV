#ifndef INTERNAL_CONDITIONS_H
#define INTERNAL_CONDITIONS_H

#include <vector>

/**
 * Calculate internal temperature and RH from ambient conditions for a batch of cases
 * 
 * @param ambient_temps Array of ambient temperatures (°C)
 * @param ambient_rhs Array of ambient relative humidity (%)
 * @param ac_voltages Array of AC voltages (V)
 * @param internal_temps Output array of internal temperatures (°C)
 * @param internal_rhs Output array of internal relative humidity (%)
 */
void calculate_internal_conditions(
    const std::vector<double>& ambient_temps,
    const std::vector<double>& ambient_rhs,
    const std::vector<double>& ac_voltages,
    std::vector<double>& internal_temps,
    std::vector<double>& internal_rhs
);

#endif // INTERNAL_CONDITIONS_H

