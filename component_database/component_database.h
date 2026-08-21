#ifndef COMPONENT_DATABASE_H
#define COMPONENT_DATABASE_H

#include <string>
#include "reliability_assessment/reliability_models.h"

// Forward declaration
struct sqlite3;

class ComponentDatabase {
public:
    ComponentDatabase();
    ~ComponentDatabase();
    
    // Initialize database connection
    bool initialize(const std::string& db_path);
    
    // Load component parameters
    bool load_capacitor(const std::string& part_number, 
                       CapacitorCoefficients& coeffs, 
                       CapacitorType& type,
                       std::string& voltage_type);
    
    bool load_power_module(const std::string& part_number, 
                          PowerModuleCoefficients& coeffs);
    
    bool load_fan_cooling(const std::string& part_number, 
                         FanCoefficients& coeffs);
    
    bool load_pcb(const std::string& part_number, 
                  PCBParameters& pcb_params);
    
    bool load_pv_inverter(const std::string& part_number,
                          PVInverterParameters& inverter_params);
    
    bool load_grid(const std::string& part_number,
                   GridParameters& grid_params);
    
private:
    sqlite3* db_;
    
    // Helper to convert string to CapacitorType
    CapacitorType string_to_capacitor_type(const std::string& type_str);
};

#endif // COMPONENT_DATABASE_H

