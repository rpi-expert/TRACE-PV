#include "reliability_models.h"

// Default coefficients initialization
// Default to FILM capacitor type
const CapacitorCoefficients DEFAULT_CAPACITOR_COEFFS = {
    CapacitorType::FILM,
    {
        {
            // Film capacitor coefficients
            5874.0,     // A: Base coefficient
            3.63,       // n: Humidity exponent
            0.55,       // Ea: Activation energy (eV)
            -0.05,      // beta: Voltage coefficient
            240.0,      // V_rated: Rated voltage (V)
            358.15,     // T_ref: Reference temperature (K)
            85.0        // RH_ref: Reference relative humidity (%)
        }
    },
    0.5,    // rth_amb: Thermal resistance from ambient to hotspot (K/W) - random placeholder value
    0.3,    // rth_surf: Thermal resistance from ambient to surface (K/W) - random placeholder value
    0.01,   // esr: Equivalent series resistance (Ohm) - random placeholder value
    330e-6  // capacitance: Capacitance (Farads) - default 330 uF
};

const PowerModuleCoefficients DEFAULT_POWER_MODULE_COEFFS = {
    {
        // deltaT model coefficients
        1e5,       // A: Base coefficient
        -4.42,     // n: Temperature range exponent
        0.042      // Ea: Activation energy (eV)
    },
    {
        // Arrhenius model coefficients
        1e6,       // A: Base coefficient
        2.5,       // n1: Temperature exponent
        1.8,       // n2: Additional exponent
        0.85,      // Ea: Activation energy (eV)
        95.0,      // RH_ref: Reference relative humidity (%)
        348.15,    // T_ref: Reference temperature (K)
        800.0      // V_ref: Reference voltage (V)
    }
};


const FanCoefficients DEFAULT_FAN_COEFFS = {
    {
        // Electrical coefficients
        1e-3,      // A: Base coefficient
        -3.06,     // n: Humidity exponent
        0.8581     // Ea: Activation energy (eV)
    },
    {
        // Mechanical coefficients
        671027.0,  // A: Base coefficient
        -0.04      // c: Temperature coefficient
    }
};

