#pragma once

#include <string>
#include <vector>

struct SimulationCase {
    std::string time;            // Timestamp string
    double ambient_temperature;  // Celsius
    double rh;                   // Relative humidity (%)
    double solar_irradiance;     // W/m^2 (GHI - Global Horizontal Irradiance)
    double ac_voltage;           // V (RMS line-to-line)
};

