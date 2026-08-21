#ifndef INTERNAL_CONDITIONS_H
#define INTERNAL_CONDITIONS_H

#include <vector>

/**
 * Calculate internal temperature and relative humidity conditions using the
 * environmental model from ddm/data_driven_model_without_trainning.ipynb.
 *
 * The DDM notebook uses a physics-informed environmental preprocessing model:
 * - Magnus formula for ambient dew point
 * - inverter waste-heat estimate from load ratio
 * - EWMA thermal inertia for internal temperature
 * - rolling ambient dew point as internal moisture inertia
 * - recombination of internal temperature and dew point into internal RH
 *
 * This pure C++ implementation is intentionally batch/vector based so the
 * simulator can precompute the full mission profile once and reuse the results
 * in GPU worker batches.
 *
 * @param ambient_temps Input: External/ambient temperatures in Celsius
 * @param ambient_rhs Input: External/ambient relative humidity in %
 * @param load_values Input: AC power in W when available, otherwise a load proxy
 *                    such as GHI. Values are converted to load ratio by
 *                    rated_load_value.
 * @param rated_load_value Positive rated/max load used to normalize load_values.
 *                         If <= 0, the max positive load value is used.
 * @param internal_temps Output: Predicted internal temperatures in Celsius
 * @param internal_rhs Output: Predicted internal relative humidity in %
 */
void calculate_internal_conditions_ddm(
    const std::vector<double>& ambient_temps,
    const std::vector<double>& ambient_rhs,
    const std::vector<double>& load_values,
    double rated_load_value,
    std::vector<double>& internal_temps,
    std::vector<double>& internal_rhs
);

/**
 * Backward-compatible wrapper. The third vector is treated as a generic load
 * proxy and normalized by its own max positive value.
 */
void calculate_internal_conditions(
    const std::vector<double>& ambient_temps,
    const std::vector<double>& ambient_rhs,
    const std::vector<double>& load_values,
    std::vector<double>& internal_temps,
    std::vector<double>& internal_rhs
);

#endif // INTERNAL_CONDITIONS_H
