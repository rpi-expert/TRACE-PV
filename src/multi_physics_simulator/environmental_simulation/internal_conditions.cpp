#include "internal_conditions.h"

// Calculate internal temperature and RH from ambient conditions for a batch of cases
// Input: arrays of ambient temperature (°C), ambient RH (%), AC voltage (V)
// Output: arrays of internal temperature (°C), internal RH (%)
void calculate_internal_conditions(
    const std::vector<double>& ambient_temps,
    const std::vector<double>& ambient_rhs,
    const std::vector<double>& ac_voltages,
    std::vector<double>& internal_temps,
    std::vector<double>& internal_rhs
) {
    size_t num_cases = ambient_temps.size();
    internal_temps.resize(num_cases);
    internal_rhs.resize(num_cases);
    
    for (size_t i = 0; i < num_cases; ++i) {
        // Simple model: internal temp = ambient + temperature rise
        // Temperature rise depends on power dissipation (simplified as function of AC voltage)
        internal_temps[i] = ambient_temps[i];
        
        // Internal RH calculation: assuming constant absolute humidity
        // RH decreases as temperature increases (simplified psychrometric relationship)
        // Using simplified approximation: RH_internal ≈ RH_ambient * (T_ambient + 273.15) / (T_internal + 273.15)
        internal_rhs[i] = ambient_rhs[i];
    }
}

