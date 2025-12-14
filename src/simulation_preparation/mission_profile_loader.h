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

#endif // MISSION_PROFILE_LOADER_H

