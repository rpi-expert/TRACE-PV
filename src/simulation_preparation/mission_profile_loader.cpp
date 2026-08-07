#include "mission_profile_loader.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace {

std::vector<std::string> split_csv_line(const std::string& line) {
    std::istringstream iss(line);
    std::string token;
    std::vector<std::string> tokens;
    while (std::getline(iss, token, ',')) {
        token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), token.end());
        tokens.push_back(token);
    }
    return tokens;
}

bool is_header_line(const std::string& line) {
    return line.find("time") != std::string::npos ||
           line.find("ambient_temperature") != std::string::npos ||
           line.find("GHI") != std::string::npos ||
           line.find("solar_irradiance") != std::string::npos ||
           line.find("ac_voltage") != std::string::npos ||
           line.find("rh") != std::string::npos;
}

} // namespace

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
        
        std::vector<std::string> tokens = split_csv_line(line);
        
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
        
        std::vector<std::string> tokens = split_csv_line(line);
        
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

std::vector<SimulationCase> load_mission_profile_csv(
    const std::string& csv_path
) {
    std::vector<SimulationCase> cases;

    std::ifstream csv_file(csv_path);
    if (!csv_file.is_open()) {
        throw std::runtime_error("Failed to open mission profile CSV: " + csv_path);
    }

    std::string line;
    bool first_line = true;

    while (std::getline(csv_file, line)) {
        if (line.empty()) {
            continue;
        }

        if (first_line) {
            first_line = false;
            if (is_header_line(line)) {
                continue;
            }
        }

        std::vector<std::string> tokens = split_csv_line(line);

        try {
            SimulationCase sc;
            if (tokens.size() >= 5) {
                sc.time = tokens[0];
                sc.ambient_temperature = std::stod(tokens[1]);
                sc.rh = std::stod(tokens[2]);
                sc.solar_irradiance = std::stod(tokens[3]);
                sc.ac_voltage = std::stod(tokens[4]);
                if (tokens.size() >= 6 && !tokens[5].empty()) {
                    sc.ac_power = std::stod(tokens[5]);
                    sc.has_ac_power = true;
                }
            } else if (tokens.size() >= 3) {
                sc.time = std::to_string(cases.size());
                sc.ambient_temperature = std::stod(tokens[0]);
                sc.rh = 50.0;
                sc.solar_irradiance = std::stod(tokens[1]);
                sc.ac_voltage = std::stod(tokens[2]);
            } else {
                continue;
            }

            if (sc.solar_irradiance <= 0.0 || sc.ac_voltage <= 0.0) {
                continue;
            }

            cases.push_back(sc);
        } catch (const std::exception&) {
            continue;
        }
    }

    return cases;
}

std::vector<SimulationCase> create_static_mission_profile(
    double ambient_temperature,
    double rh,
    double ac_voltage,
    double ac_power,
    int num_cases,
    double solar_irradiance
) {
    if (num_cases <= 0) {
        throw std::invalid_argument("Static mission profile case count must be greater than 0");
    }

    std::vector<SimulationCase> cases;
    cases.reserve(num_cases);

    for (int i = 0; i < num_cases; ++i) {
        SimulationCase sc;
        sc.time = "static_" + std::to_string(i);
        sc.ambient_temperature = ambient_temperature;
        sc.rh = rh;
        sc.solar_irradiance = solar_irradiance;
        sc.ac_voltage = ac_voltage;
        sc.ac_power = ac_power;
        sc.has_ac_power = true;
        cases.push_back(sc);
    }

    return cases;
}
