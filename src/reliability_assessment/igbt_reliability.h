#ifndef IGBT_RELIABILITY_H
#define IGBT_RELIABILITY_H

#include "reliability_models.h"
#include <vector>

/**
 * Calculate IGBT (power module) lifetime based on reliability model
 * 
 * @param delta_range Temperature ranges from rainflow counting
 * @param delta_cycle Number of cycles for each range
 * @param Tj_max Maximum junction temperature (Celsius)
 * @param coeffs Power module coefficients
 * @return Lifetime in hours
 */
double calculate_igbt_lifetime(
    const std::vector<double>& delta_range,
    const std::vector<double>& delta_cycle,
    double Tj_max,
    const PowerModuleCoefficients& coeffs
);

/**
 * Calculate Nf (cycles to failure) for IGBT using deltaT model
 * 
 * @param deltaT Temperature range (Celsius)
 * @param Tj_max Maximum junction temperature (Celsius)
 * @param coeffs Power module coefficients
 * @return Nf (number of cycles to failure)
 */
double calculate_igbt_nf_deltaT(
    double deltaT,
    double Tj_max,
    const PowerModuleCoefficients& coeffs
);

/**
 * Calculate IGBT lifetime using Arrhenius model (corrosion/dendrites model)
 * 
 * @param T_internal Internal temperature in Celsius (from internal conditions calculation)
 * @param rh Relative humidity in % (from mission profile)
 * @param voltage AC voltage in V (from mission profile)
 * @param coeffs Power module coefficients (contains RH_ref, T_ref, V_ref from IGBT profile)
 * @return Lifetime in hours
 */
double calculate_igbt_lifetime_arrhenius(
    double T_internal,
    double rh,
    double voltage,
    const PowerModuleCoefficients& coeffs
);

#endif // IGBT_RELIABILITY_H

