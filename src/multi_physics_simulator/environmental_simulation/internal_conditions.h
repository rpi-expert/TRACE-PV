#ifndef INTERNAL_CONDITIONS_H
#define INTERNAL_CONDITIONS_H

#include <vector>

/**
 * Calculate internal temperature and relative humidity conditions
 * based on ambient conditions and AC voltage using trained ML models.
 * 
 * This function uses machine learning models to predict internal conditions
 * inside the power converter based on:
 * - Ambient temperature
 * - Ambient relative humidity  
 * - AC voltage (converted to power ratio)
 * 
 * @param ambient_temps Input: Ambient temperatures in Celsius
 * @param ambient_rhs Input: Ambient relative humidity in %
 * @param ac_voltages Input: AC voltages in Volts
 * @param internal_temps Output: Predicted internal temperatures in Celsius
 * @param internal_rhs Output: Predicted internal relative humidity in %
 */
void calculate_internal_conditions(
    const std::vector<double>& ambient_temps,
    const std::vector<double>& ambient_rhs,
    const std::vector<double>& ac_voltages,
    std::vector<double>& internal_temps,
    std::vector<double>& internal_rhs
);

#endif // INTERNAL_CONDITIONS_H

