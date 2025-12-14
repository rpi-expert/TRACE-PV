#pragma once

// Capacitor thermal calculation results
struct CapacitorThermalResult {
    double capacitor_hotspot_temperature;  // Capacitor hotspot temperature (°C)
    double capacitor_surface_temperature;  // Capacitor surface temperature (°C)
};

// Power module thermal calculation results
struct PowerModuleThermalResult {
    double junction_temperature;  // Junction temperature (°C)
};

// Calculate capacitor thermal response from capacitor losses
// Input: Capacitor loss, internal temperature, rth_amb (thermal resistance ambient to hotspot), rth_surf (thermal resistance ambient to surface)
// Output: Capacitor hotspot temperature and capacitor surface temperature
// hotspot_temp = internal_temp + power_loss * rth_amb
// surface_temp = internal_temp + power_loss * rth_surf
CapacitorThermalResult calculate_capacitor_thermal(double capacitor_loss,
                                                    double internal_temperature,
                                                    double rth_amb,
                                                    double rth_surf);

// Calculate power module thermal response from ac_power
// Input: AC power, ambient temperature
// Output: Junction temperature
// Formula: Tj = T_amb + ac_power / 30000 * (100 - 45)
PowerModuleThermalResult calculate_power_module_thermal(double ac_power,
                                                        double ambient_temperature);

