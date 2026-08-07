#ifndef MISSION_PROFILE_LOADER_H
#define MISSION_PROFILE_LOADER_H

#include "simulation_case.h"
#include <string>
#include <vector>

/**
 * Load mission profile from two separate CSV files:
 * - Environmental conditions: ambient_temperature, rh, GHI
 * - Operating conditions: ac_voltage
 * 
 * Filters out records where GHI <= 0 or ac_voltage <= 0
 * 
 * @param environmental_csv_path Path to environmental mission profile CSV
 * @param operating_csv_path Path to operating mission profile CSV
 * @return Vector of SimulationCase objects (filtered)
 */
std::vector<SimulationCase> load_mission_profile(
    const std::string& environmental_csv_path,
    const std::string& operating_csv_path
);

/**
 * Load mission profile from one CSV file.
 *
 * Supported formats:
 * - time,ambient_temperature,rh,GHI,ac_voltage[,ac_power]
 * - ambient_temperature,solar_irradiance,ac_voltage
 */
std::vector<SimulationCase> load_mission_profile_csv(
    const std::string& csv_path
);

/**
 * Create repeated static simulation cases.
 */
std::vector<SimulationCase> create_static_mission_profile(
    double ambient_temperature,
    double rh,
    double ac_voltage,
    double ac_power,
    int num_cases,
    double solar_irradiance = 1000.0
);

#endif // MISSION_PROFILE_LOADER_H
