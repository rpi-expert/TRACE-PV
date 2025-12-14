#include "mission_profile_loader.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>

std::vector<SimulationCase> load_mission_profile(
    const std::string& environmental_csv_path,
    const std::string& operating_csv_path
) {
    std::vector<SimulationCase> cases;
    
    // Open environmental CSV file
    std::ifstream env_file(environmental_csv_path);
    if (!env_file.is_open()) {
        throw std::runtime_error("Failed to open environmental mission profile CSV: " + environmental_csv_path);
    }
    
    // Open operating CSV file
    std::ifstream op_file(operating_csv_path);
    if (!op_file.is_open()) {
        throw std::runtime_error("Failed to open operating mission profile CSV: " + operating_csv_path);
    }
    
    // Read environmental data
    std::vector<std::string> env_times;
    std::vector<double> env_temps;
    std::vector<double> env_rhs;
    std::vector<double> env_ghis;
    
    std::string line;
    bool first_line = true;
    
    // Read environmental CSV: time,ambient_temperature,rh,GHI
    while (std::getline(env_file, line)) {
        // Skip header line
        if (first_line) {
            first_line = false;
            if (line.find("time") != std::string::npos || 
                line.find("ambient_temperature") != std::string::npos ||
                line.find("GHI") != std::string::npos ||
                line.find("rh") != std::string::npos) {
                continue;
            }
        }
        
        // Skip empty lines
        if (line.empty()) {
            continue;
        }
        
        std::istringstream iss(line);
        std::string token;
        std::vector<std::string> tokens;
        
        while (std::getline(iss, token, ',')) {
            tokens.push_back(token);
        }
        
        // CSV format: time,ambient_temperature,rh,GHI
        if (tokens.size() >= 4) {
            try {
                env_times.push_back(tokens[0]);
                env_temps.push_back(std::stod(tokens[1]));
                env_rhs.push_back(std::stod(tokens[2]));
                env_ghis.push_back(std::stod(tokens[3]));
            } catch (const std::exception& e) {
                // Skip invalid lines
                continue;
            }
        }
    }
    
    // Read operating data
    std::vector<std::string> op_times;
    std::vector<double> op_voltages;
    
    first_line = true;
    
    // Read operating CSV: time,ac_voltage
    while (std::getline(op_file, line)) {
        // Skip header line
        if (first_line) {
            first_line = false;
            if (line.find("time") != std::string::npos || 
                line.find("ac_voltage") != std::string::npos) {
                continue;
            }
        }
        
        // Skip empty lines
        if (line.empty()) {
            continue;
        }
        
        std::istringstream iss(line);
        std::string token;
        std::vector<std::string> tokens;
        
        while (std::getline(iss, token, ',')) {
            tokens.push_back(token);
        }
        
        // CSV format: time,ac_voltage
        if (tokens.size() >= 2) {
            try {
                op_times.push_back(tokens[0]);
                op_voltages.push_back(std::stod(tokens[1]));
            } catch (const std::exception& e) {
                // Skip invalid lines
                continue;
            }
        }
    }
    
    // Check that both files have the same number of records
    if (env_times.size() != op_times.size()) {
        throw std::runtime_error("Mismatch in number of records: environmental=" + 
                                 std::to_string(env_times.size()) + 
                                 ", operating=" + std::to_string(op_times.size()));
    }
    
    // Combine data and filter out records with GHI <= 0 or ac_voltage <= 0
    for (size_t i = 0; i < env_times.size(); ++i) {
        // Filter: skip if GHI <= 0 or ac_voltage <= 0
        if (env_ghis[i] <= 0.0 || op_voltages[i] <= 0.0) {
            continue;
        }
        
        // Verify time stamps match (optional check)
        if (env_times[i] != op_times[i]) {
            // Warning but continue - times should match but not critical
            // std::cerr << "Warning: Time mismatch at index " << i << std::endl;
        }
        
        SimulationCase sc;
        sc.time = env_times[i];
        sc.ambient_temperature = env_temps[i];
        sc.rh = env_rhs[i];
        sc.solar_irradiance = env_ghis[i];  // GHI
        sc.ac_voltage = op_voltages[i];
        cases.push_back(sc);
    }
    
    return cases;
}

