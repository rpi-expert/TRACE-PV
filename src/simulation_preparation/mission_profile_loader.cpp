#include "mission_profile_loader.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <cmath>
#include <iostream>
#include <map>

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

// -1 C is the field missing-data sentinel. Other subzero temperatures are valid.
// Near-zero RH means <=1 percent, not a fractional RH value.
bool valid_mission_input(const SimulationCase& sc) {
    return std::isfinite(sc.ambient_temperature) && sc.ambient_temperature != -1.0 &&
           std::isfinite(sc.rh) && sc.rh > 1.0 && sc.rh <= 100.0 &&
           std::isfinite(sc.solar_irradiance) && sc.solar_irradiance > 0.0 &&
           std::isfinite(sc.ac_voltage) && sc.ac_voltage > 0.0 &&
           (!sc.has_ac_power || std::isfinite(sc.ac_power)) &&
           (!sc.has_internal_temperature ||
            (std::isfinite(sc.internal_temperature) && sc.internal_temperature != -1.0));
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
    
    // Join by timestamp before filtering: malformed rows must never shift alignment.
    std::map<std::string, double> voltage_by_time;
    std::string line;
    std::size_t rejected = 0;
    while (std::getline(op_file, line)) {
        if (line.empty() || is_header_line(line)) continue;
        const auto tokens = split_csv_line(line);
        try {
            if (tokens.size() < 2) throw std::invalid_argument("columns");
            const double voltage = std::stod(tokens[1]);
            if (!voltage_by_time.emplace(tokens[0], voltage).second)
                throw std::runtime_error("Duplicate operating timestamp: " + tokens[0]);
        } catch (const std::invalid_argument&) { ++rejected; }
          catch (const std::out_of_range&) { ++rejected; }
    }
    while (std::getline(env_file, line)) {
        if (line.empty() || is_header_line(line)) continue;
        const auto tokens = split_csv_line(line);
        try {
            if (tokens.size() < 4) throw std::invalid_argument("columns");
            const auto op = voltage_by_time.find(tokens[0]);
            if (op == voltage_by_time.end()) { ++rejected; continue; }
            SimulationCase sc;
            sc.time = tokens[0];
            sc.ambient_temperature = std::stod(tokens[1]);
            sc.rh = std::stod(tokens[2]);
            sc.solar_irradiance = std::stod(tokens[3]);
            sc.ac_voltage = op->second;
            if (!valid_mission_input(sc)) { ++rejected; continue; }
            cases.push_back(sc);
        } catch (const std::exception&) { ++rejected; }
    }
    std::cout << "TRACEPV_INPUT retained=" << cases.size() << " rejected=" << rejected
              << " (non-operating, invalid, or unmatched records; RH must exceed 1%)\n";
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
    std::size_t rejected = 0;

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
                if (tokens.size() >= 7 && !tokens[6].empty()) {
                    sc.internal_temperature = std::stod(tokens[6]);
                    sc.has_internal_temperature = true;
                }
            } else if (tokens.size() >= 3) {
                sc.time = std::to_string(cases.size());
                sc.ambient_temperature = std::stod(tokens[0]);
                sc.rh = 50.0;
                sc.solar_irradiance = std::stod(tokens[1]);
                sc.ac_voltage = std::stod(tokens[2]);
            } else {
                ++rejected;
                continue;
            }

            if (!valid_mission_input(sc)) {
                ++rejected;
                continue;
            }

            cases.push_back(sc);
        } catch (const std::exception&) {
            ++rejected;
            continue;
        }
    }

    std::cout << "TRACEPV_INPUT retained=" << cases.size() << " rejected=" << rejected
              << " (non-operating or invalid records; RH must exceed 1%)\n";
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
        const int total_minutes = i * 5;
        const int day = total_minutes / (24 * 60) + 1;
        const int minute_of_day = total_minutes % (24 * 60);
        const int hour = minute_of_day / 60;
        const int minute = minute_of_day % 60;
        std::ostringstream time_label;
        time_label << "static_day_" << day << "_"
                   << std::setw(2) << std::setfill('0') << hour
                   << ":" << std::setw(2) << std::setfill('0') << minute;

        SimulationCase sc;
        sc.time = time_label.str();
        sc.ambient_temperature = ambient_temperature;
        sc.rh = rh;
        sc.solar_irradiance = solar_irradiance;
        sc.ac_voltage = ac_voltage;
        sc.ac_power = ac_power;
        sc.has_ac_power = true;
        if (!valid_mission_input(sc))
            throw std::invalid_argument("Invalid static mission input: finite values, temperature != -1 C, RH >1 and <=100%, positive GHI/voltage required");
        cases.push_back(sc);
    }

    return cases;
}
