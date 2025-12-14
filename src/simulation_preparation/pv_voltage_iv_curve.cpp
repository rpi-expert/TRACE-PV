#include "pv_voltage_iv_curve.h"
#include <algorithm>
#include <cmath>

// Calculate PV voltage from ambient temperature and solar irradiance
// Simple model: V_pv = V_base * (1 + temp_coeff * (T - T_ref)) * (G / G_ref)
// Where temp_coeff = -0.004/°C, T_ref = 25°C, G_ref = 1000 W/m²
double calculate_pv_voltage(double ambient_temp, double solar_irradiance, double base_voltage /* = 500.0 */) {
    const double temp_coeff = -0.004;  // per °C
    const double T_ref = 25.0;         // °C
    const double G_ref = 1000.0;       // W/m²
    
    double temp_factor = 1.0 + temp_coeff * (ambient_temp - T_ref);
    double irradiance_factor = solar_irradiance / G_ref;
    
    // Clamp irradiance factor to reasonable range
    irradiance_factor = std::max(0.1, std::min(1.5, irradiance_factor));
    
    return base_voltage * temp_factor * irradiance_factor;
}

// Update simulation parameters based on simulation case and IV curve data
void update_params_for_case(SimulationParameters& params, 
                            const SimulationCase& sc,
                            const IVCurveData& iv_data) {
    // Use IV curve data if available, otherwise fall back to simple model
    if (iv_data.valid) {
        params.v_pv = iv_data.pv_voltage;
        // Note: pv_current could be used for current source modeling if needed
    } else {
        // Fall back to simple PV voltage model
        params.v_pv = calculate_pv_voltage(sc.ambient_temperature, sc.solar_irradiance);
    }
    
    // Update AC voltage (vg_mag is peak phase voltage, ac_voltage is RMS line-to-line)
    // vg_mag = (ac_voltage / sqrt(3)) * sqrt(2) = ac_voltage * sqrt(2/3)
    params.vg_mag = sc.ac_voltage * std::sqrt(2.0 / 3.0);
    
    // Recalculate boost duty if stage 2
    if (params.model_stage == 2) {
        params.boost_duty = (params.vdc_target > params.v_pv) ? 
            (1.0 - params.v_pv / params.vdc_target) : 0.0;
    }
}

