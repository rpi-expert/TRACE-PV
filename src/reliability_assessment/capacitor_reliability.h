#ifndef CAPACITOR_RELIABILITY_H
#define CAPACITOR_RELIABILITY_H

#include "reliability_models.h"

/**
 * Calculate capacitor lifetime based on reliability model
 * 
 * @param dc_voltage Applied DC voltage (V)
 * @param hotspot_temp Hotspot temperature (Celsius)
 * @param internal_rh Internal relative humidity (%)
 * @param capacitor_type Type of capacitor
 * @param coeffs Capacitor coefficients
 * @return Lifetime in hours
 */
double calculate_capacitor_lifetime(
    double dc_voltage,
    double hotspot_temp,
    double internal_rh,
    CapacitorType capacitor_type,
    const CapacitorCoefficients& coeffs
);

#endif // CAPACITOR_RELIABILITY_H

