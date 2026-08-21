#pragma once

#include <string>
#include <vector>

struct SimulationCase {
    std::string time;            // Timestamp string
    double ambient_temperature;  // Celsius
    double rh;                   // Relative humidity (%)
    double solar_irradiance;     // W/m^2 (GHI - Global Horizontal Irradiance)
    double ac_voltage;           // V (RMS line-to-line)
    double ac_power = 0.0;        // W, positive when exported to the grid
    bool has_ac_power = false;    // True when measured/provided power is available
    double internal_temperature = 0.0;  // Celsius, optional measured/local thermal boundary
    bool has_internal_temperature = false;
};
