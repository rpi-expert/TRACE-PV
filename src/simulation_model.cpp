#include "simulation_model.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cctype>

namespace {
    // Simple JSON value extractor
    std::string extract_json_string(const std::string& json, const std::string& key) {
        std::string search_key = "\"" + key + "\"";
        size_t pos = json.find(search_key);
        if (pos == std::string::npos) return "";
        
        pos = json.find(":", pos);
        if (pos == std::string::npos) return "";
        pos++;
        
        // Skip whitespace
        while (pos < json.length() && std::isspace(json[pos])) pos++;
        
        // Check for string (quoted)
        if (pos < json.length() && json[pos] == '"') {
            pos++;
            size_t end = json.find('"', pos);
            if (end == std::string::npos) return "";
            return json.substr(pos, end - pos);
        }
        
        return "";
    }
}

bool load_simulation_model(const std::string& json_file, SimulationModel& model) {
    std::ifstream file(json_file);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open simulation model file: " << json_file << std::endl;
        return false;
    }
    
    // Read entire file
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json_content = buffer.str();
    file.close();
    
    // Check for simulation_model key
    if (json_content.find("\"simulation_model\"") == std::string::npos) {
        std::cerr << "Error: Missing 'simulation_model' key in JSON" << std::endl;
        return false;
    }
    
    // Extract part numbers from nested structure
    // Look for "capacitor": { "part_number": "..." }
    size_t pos = 0;
    
    // Extract capacitor part number
    pos = json_content.find("\"capacitor\"", pos);
    if (pos != std::string::npos) {
        model.capacitor_part_number = extract_json_string(json_content.substr(pos), "part_number");
    }
    
    // Extract power_module part number
    pos = json_content.find("\"power_module\"", 0);
    if (pos != std::string::npos) {
        model.power_module_part_number = extract_json_string(json_content.substr(pos), "part_number");
    }
    
    // Extract fan_cooling part number
    pos = json_content.find("\"fan_cooling\"", 0);
    if (pos != std::string::npos) {
        model.fan_cooling_part_number = extract_json_string(json_content.substr(pos), "part_number");
    }
    
    // Extract pcb part number
    pos = json_content.find("\"pcb\"", 0);
    if (pos != std::string::npos) {
        model.pcb_part_number = extract_json_string(json_content.substr(pos), "part_number");
    }
    
    // Extract pv_panel part number
    pos = json_content.find("\"pv_panel\"", 0);
    if (pos != std::string::npos) {
        model.pv_panel_part_number = extract_json_string(json_content.substr(pos), "part_number");
        const auto panel = json_content.substr(pos, json_content.find('}', pos) - pos);
        const auto iv_path = extract_json_string(panel, "iv_database");
        if (!iv_path.empty()) model.iv_database_path = iv_path;
        for (const auto& key : {"modules_per_string", "parallel_strings"}) {
            const auto at = panel.find(std::string("\"") + key + "\"");
            if (at == std::string::npos) continue;
            try {
                const int count = std::stoi(panel.substr(panel.find(':', at) + 1));
                if (count <= 0) return false;
                if (std::string(key) == "modules_per_string") model.pv_modules_per_string = count;
                else model.pv_parallel_strings = count;
            } catch (...) { return false; }
        }

    }
    
    // Extract pv_inverter part number
    pos = json_content.find("\"pv_inverter\"", 0);
    if (pos != std::string::npos) {
        model.pv_inverter_part_number = extract_json_string(json_content.substr(pos), "part_number");
    }
    
    // Extract grid part number
    pos = json_content.find("\"grid\"", 0);
    if (pos != std::string::npos) {
        model.grid_part_number = extract_json_string(json_content.substr(pos), "part_number");
    }
    
    return true;
}

