#ifndef PV_VOLTAGE_IV_CURVE_H
#define PV_VOLTAGE_IV_CURVE_H

#include "simulation_params.h"
#include "simulation_case.h"
#include <vector>

// Structure to store IV curve data for a simulation case
// This will be populated from mission profile and used in simulation
struct IVCurveData {
    double voc = 0.0;
    double isc = 0.0;
    double pv_voltage = 0.0;  // Operating voltage (e.g., at MPP or DC-link voltage)
    double pv_current = 0.0;  // Current at operating voltage
    bool valid = false;
    
    IVCurveData() : voc(0.0), isc(0.0), pv_voltage(0.0), pv_current(0.0), valid(false) {}
};

/**
 * Calculate PV voltage from ambient temperature and solar irradiance
 * Simple model: V_pv = V_mp0 * (1 + alpha * (T - T_ref)) * (G / G_ref)
 * 
 * @param ambient_temp Ambient temperature (°C)
 * @param solar_irradiance Solar irradiance (W/m²)
 * @param base_voltage Base voltage (V), default is 500.0
 * @return PV voltage (V)
 */
double calculate_pv_voltage(double ambient_temp, double solar_irradiance, double base_voltage = 500.0);

/**
 * Update simulation parameters based on simulation case and IV curve data
 * 
 * @param params Simulation parameters to update
 * @param sc Simulation case containing mission profile data
 * @param iv_data IV curve data for this case
 */
void update_params_for_case(SimulationParameters& params, 
                            const SimulationCase& sc,
                            const IVCurveData& iv_data);

#endif // PV_VOLTAGE_IV_CURVE_H

