#pragma once

#include <string>

/**
 * Simulation Model Structure
 * Contains part numbers for all components in the simulation
 */
struct SimulationModel {
    std::string capacitor_part_number;
    std::string power_module_part_number;
    std::string fan_cooling_part_number;
    std::string pcb_part_number;
    std::string pv_panel_part_number;
    std::string pv_inverter_part_number;
    std::string grid_part_number;
    
    // Default constructor
    SimulationModel() = default;
};

/**
 * Load simulation model from JSON file
 * @param json_file Path to simulation model JSON file
 * @param model Output simulation model structure
 * @return true if successful, false otherwise
 */
bool load_simulation_model(const std::string& json_file, SimulationModel& model);

