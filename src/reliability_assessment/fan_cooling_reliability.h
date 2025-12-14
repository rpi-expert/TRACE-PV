#ifndef FAN_COOLING_RELIABILITY_H
#define FAN_COOLING_RELIABILITY_H

#include "reliability_models.h"

/**
 * Calculate cooling fan lifetime using electrical model
 * Lifetime_electrical = A * RH_op^n * exp(-E_a / (k_B * T_op))
 * 
 * @param temperature Operating temperature (Celsius)
 * @param relative_humidity Operating relative humidity (%)
 * @param coeffs Fan coefficients
 * @return Lifetime in hours
 */
double calculate_fan_lifetime_electrical(
    double temperature,
    double relative_humidity,
    const FanCoefficients& coeffs
);

/**
 * Calculate cooling fan lifetime using mechanical model
 * Lifetime_mechanical = A * exp(c * T_op)
 * 
 * @param temperature Operating temperature (Celsius)
 * @param coeffs Fan coefficients
 * @return Lifetime in hours
 */
double calculate_fan_lifetime_mechanical(
    double temperature,
    const FanCoefficients& coeffs
);

#endif // FAN_COOLING_RELIABILITY_H

