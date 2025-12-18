#include "internal_conditions.h"

#include <vector>
#include <cmath>
#include <algorithm>

// Define the rated voltage used during training
// This must match the value used to calculate 'power_ratio' during model training
// Typical values: 240V (residential) or 480V (commercial/industrial)
const double RATED_VOLTAGE = 240.0; 

void calculate_internal_conditions(
    const std::vector<double>& ambient_temps,
    const std::vector<double>& ambient_rhs,
    const std::vector<double>& ac_voltages,
    std::vector<double>& internal_temps,
    std::vector<double>& internal_rhs
) {
    // Validate input sizes
    size_t num_cases = ambient_temps.size();
    if (ambient_rhs.size() != num_cases || ac_voltages.size() != num_cases) {
        // If sizes don't match, resize output vectors to zero and return
        internal_temps.clear();
        internal_rhs.clear();
        return;
    }
    
    // Resize output vectors
    internal_temps.resize(num_cases);
    internal_rhs.resize(num_cases);

    // Process each case
    for (size_t i = 0; i < num_cases; ++i) {
     
        internal_rhs[i] = ambient_rhs[i] - ac_voltages[i] / RATED_VOLTAGE * 40;
        internal_temps[i] = ambient_temps[i] +ac_voltages[i] / RATED_VOLTAGE * 20;
    }
}