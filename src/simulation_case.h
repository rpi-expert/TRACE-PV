#pragma once

#include <string>
#include <vector>

struct SimulationCase {
    std::string time;            // Timestamp string
    double ambient_temperature;  // Celsius
    double rh;                   // Relative humidity (%)
    double solar_irradiance;     // W/m^2 (GHI - Global Horizontal Irradiance)
    double ac_voltage;           // V (RMS line-to-line)
    double ac_power = 0.0;        // W, optional override for thermal model
    bool has_ac_power = false;    // True when ac_power should override calculated power
};
