#include "component_database/component_database.h"
#include <sqlite3.h>
#include <cstring>
#include <iostream>

ComponentDatabase::ComponentDatabase() : db_(nullptr) {}

ComponentDatabase::~ComponentDatabase() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool ComponentDatabase::initialize(const std::string& db_path) {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "Error opening database: " << sqlite3_errmsg(db_) << std::endl;
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }
    
    return true;
}

CapacitorType ComponentDatabase::string_to_capacitor_type(const std::string& type_str) {
    if (type_str == "FILM") return CapacitorType::FILM;
    if (type_str == "ALUMINUM_ELECTROLYTIC") return CapacitorType::ALUMINUM_ELECTROLYTIC;
    if (type_str == "TANTALUM") return CapacitorType::TANTALUM;
    if (type_str == "CERAMIC") return CapacitorType::CERAMIC;
    return CapacitorType::ALUMINUM_ELECTROLYTIC; // Default
}

bool ComponentDatabase::load_capacitor(const std::string& part_number, 
                                       CapacitorCoefficients& coeffs, 
                                       CapacitorType& type,
                                       std::string& voltage_type) {
    if (!db_) return false;
    
    const char* sql = "SELECT capacitor_type, voltage_type, "
                     "A, n, Ea, beta, V_rated, T_ref, RH_ref, "
                     "L0, V0, T0, beta_min, beta_max, "
                     "rth_amb, rth_surf, esr, capacitance "
                     "FROM capacitor WHERE part_number = ?";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, part_number.c_str(), -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }
    
    // Get capacitor type
    const char* type_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    type = string_to_capacitor_type(type_str ? type_str : "");
    coeffs.type = type;
    
    // Get voltage type
    const char* vtype_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    voltage_type = vtype_str ? vtype_str : "DC";
    
    if (type == CapacitorType::FILM) {
        // Film capacitor parameters (columns 2-8)
        coeffs.coeffs.film.A = sqlite3_column_double(stmt, 2);
        coeffs.coeffs.film.n = sqlite3_column_double(stmt, 3);
        coeffs.coeffs.film.Ea = sqlite3_column_double(stmt, 4);
        coeffs.coeffs.film.beta = sqlite3_column_double(stmt, 5);
        coeffs.coeffs.film.V_rated = sqlite3_column_double(stmt, 6);
        coeffs.coeffs.film.T_ref = sqlite3_column_double(stmt, 7);
        coeffs.coeffs.film.RH_ref = sqlite3_column_double(stmt, 8);
    } else if (type == CapacitorType::ALUMINUM_ELECTROLYTIC) {
        // Aluminum electrolytic parameters (columns 9-13)
        coeffs.coeffs.aluminum.L0 = sqlite3_column_double(stmt, 9);
        coeffs.coeffs.aluminum.V0 = sqlite3_column_double(stmt, 10);
        coeffs.coeffs.aluminum.T0 = sqlite3_column_double(stmt, 11);
        coeffs.coeffs.aluminum.beta_min = sqlite3_column_double(stmt, 12);
        coeffs.coeffs.aluminum.beta_max = sqlite3_column_double(stmt, 13);
    }
    
    // Thermal and electrical parameters (columns 14-17)
    coeffs.rth_amb = sqlite3_column_double(stmt, 14);
    coeffs.rth_surf = sqlite3_column_double(stmt, 15);
    coeffs.esr = sqlite3_column_double(stmt, 16);
    coeffs.capacitance = sqlite3_column_double(stmt, 17);
    
    // Use defaults if NULL values
    if (coeffs.rth_amb == 0.0 && sqlite3_column_type(stmt, 14) == SQLITE_NULL) {
        coeffs.rth_amb = 0.5;
    }
    if (coeffs.rth_surf == 0.0 && sqlite3_column_type(stmt, 15) == SQLITE_NULL) {
        coeffs.rth_surf = 0.3;
    }
    if (coeffs.esr == 0.0 && sqlite3_column_type(stmt, 16) == SQLITE_NULL) {
        coeffs.esr = 0.01;
    }
    if (coeffs.capacitance == 0.0 && sqlite3_column_type(stmt, 17) == SQLITE_NULL) {
        coeffs.capacitance = 330e-6;  // Default 330 uF
    }
    
    sqlite3_finalize(stmt);
    return true;
}

bool ComponentDatabase::load_power_module(const std::string& part_number, 
                                         PowerModuleCoefficients& coeffs) {
    if (!db_) return false;
    
    const char* sql = "SELECT deltaT_A, deltaT_n, deltaT_Ea, arrhenius_A, arrhenius_n1, arrhenius_n2, arrhenius_Ea, arrhenius_RH_ref, arrhenius_T_ref, arrhenius_V_ref FROM power_module WHERE part_number = ?";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, part_number.c_str(), -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }
    
    // Load deltaT model coefficients
    coeffs.deltaT_model.A = sqlite3_column_double(stmt, 0);
    coeffs.deltaT_model.n = sqlite3_column_double(stmt, 1);
    coeffs.deltaT_model.Ea = sqlite3_column_double(stmt, 2);
    
    // Load Arrhenius model coefficients
    coeffs.arrhenius_model.A = sqlite3_column_double(stmt, 3);
    coeffs.arrhenius_model.n1 = sqlite3_column_double(stmt, 4);
    coeffs.arrhenius_model.n2 = sqlite3_column_double(stmt, 5);
    coeffs.arrhenius_model.Ea = sqlite3_column_double(stmt, 6);
    coeffs.arrhenius_model.RH_ref = sqlite3_column_double(stmt, 7);
    coeffs.arrhenius_model.T_ref = sqlite3_column_double(stmt, 8);
    coeffs.arrhenius_model.V_ref = sqlite3_column_double(stmt, 9);
    
    sqlite3_finalize(stmt);
    return true;
}

bool ComponentDatabase::load_fan_cooling(const std::string& part_number, 
                                         FanCoefficients& coeffs) {
    if (!db_) return false;
    
    const char* sql = "SELECT A_electrical, n, Ea, "
                     "A_mechanical, c "
                     "FROM fan_cooling WHERE part_number = ?";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, part_number.c_str(), -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }
    
    coeffs.electrical.A = sqlite3_column_double(stmt, 0);
    coeffs.electrical.n = sqlite3_column_double(stmt, 1);
    coeffs.electrical.Ea = sqlite3_column_double(stmt, 2);
    coeffs.mechanical.A = sqlite3_column_double(stmt, 3);
    coeffs.mechanical.c = sqlite3_column_double(stmt, 4);
    
    sqlite3_finalize(stmt);
    return true;
}

bool ComponentDatabase::load_pcb(const std::string& part_number, 
                                 PCBParameters& pcb_params) {
    if (!db_) return false;
    
    const char* sql = "SELECT component_type, material, "
                     "component_length, component_width, component_thickness, "
                     "copper_length, copper_width, copper_thickness, "
                     "solder_length, solder_width, solder_thickness, "
                     "ductility_A, ductility_B, ductility_C, ductility_D, ductility_E, "
                     "shear_modulus, CTE_component, G_copper, G_FR4, CTE_FR4, E_FR4, Poisson_FR4, "
                     "pcb_thickness, max_temperature, min_temperature, dwell_time, ramp_time, "
                     "beta, lifetime_N63, adjust_param "
                     "FROM pcb WHERE part_number = ?";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, part_number.c_str(), -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }
    
    // Read component_type and material
    const char* type_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    pcb_params.component_type = type_str ? type_str : "";
    const char* material_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    pcb_params.material = material_str ? material_str : "";
    
    // Component dimensions
    pcb_params.component_dims.length = sqlite3_column_double(stmt, 2);
    pcb_params.component_dims.width = sqlite3_column_double(stmt, 3);
    pcb_params.component_dims.thickness = sqlite3_column_double(stmt, 4);
    
    // Copper dimensions
    pcb_params.copper_dims.length = sqlite3_column_double(stmt, 5);
    pcb_params.copper_dims.width = sqlite3_column_double(stmt, 6);
    pcb_params.copper_dims.thickness = sqlite3_column_double(stmt, 7);
    
    // Solder dimensions
    pcb_params.solder_dims.length = sqlite3_column_double(stmt, 8);
    pcb_params.solder_dims.width = sqlite3_column_double(stmt, 9);
    pcb_params.solder_dims.thickness = sqlite3_column_double(stmt, 10);
    
    // Ductility coefficients
    pcb_params.ductility.A = sqlite3_column_double(stmt, 11);
    pcb_params.ductility.B = sqlite3_column_double(stmt, 12);
    pcb_params.ductility.C = sqlite3_column_double(stmt, 13);
    pcb_params.ductility.D = sqlite3_column_double(stmt, 14);
    pcb_params.ductility.E = sqlite3_column_double(stmt, 15);
    
    // Material properties
    pcb_params.shear_modulus = sqlite3_column_double(stmt, 16);
    pcb_params.CTE_component = sqlite3_column_double(stmt, 17);
    pcb_params.G_copper = sqlite3_column_double(stmt, 18);
    pcb_params.G_FR4 = sqlite3_column_double(stmt, 19);
    pcb_params.CTE_FR4 = sqlite3_column_double(stmt, 20);
    pcb_params.E_FR4 = sqlite3_column_double(stmt, 21);
    pcb_params.Poisson_FR4 = sqlite3_column_double(stmt, 22);
    pcb_params.pcb_thickness = sqlite3_column_double(stmt, 23);
    
    // Thermal cycling parameters
    pcb_params.max_temperature = sqlite3_column_double(stmt, 24);
    pcb_params.min_temperature = sqlite3_column_double(stmt, 25);
    pcb_params.dwell_time = sqlite3_column_double(stmt, 26);
    pcb_params.ramp_time = sqlite3_column_double(stmt, 27);
    
    // Lifetime parameters
    pcb_params.beta = sqlite3_column_double(stmt, 28);
    pcb_params.lifetime_N63 = sqlite3_column_double(stmt, 29);
    pcb_params.adjust_param = sqlite3_column_double(stmt, 30);
    
    sqlite3_finalize(stmt);
    return true;
}

bool ComponentDatabase::load_pv_inverter(const std::string& part_number,
                                         PVInverterParameters& inverter_params) {
    if (!db_) return false;
    
    const char* sql = "SELECT boost_included, model_stage, topology_level, modulation_scheme, "
                     "switching_frequency, Vdc, CDC1, CDC2, "
                     "L1, R1, C1, L2, R2, C2, "
                     "Lboost, RLboost, Cboost, boost_frequency "
                     "FROM pv_inverter WHERE part_number = ?";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, part_number.c_str(), -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }
    
    inverter_params.part_number = part_number;
    inverter_params.boost_included = (sqlite3_column_int(stmt, 0) != 0);
    inverter_params.model_stage = sqlite3_column_int(stmt, 1);
    inverter_params.topology_level = sqlite3_column_int(stmt, 2);
    
    const char* mod_scheme = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    inverter_params.modulation_scheme = mod_scheme ? mod_scheme : "SVM";
    
    inverter_params.switching_frequency = sqlite3_column_double(stmt, 4);
    inverter_params.Vdc = sqlite3_column_double(stmt, 5);
    inverter_params.CDC1 = sqlite3_column_double(stmt, 6);
    inverter_params.CDC2 = sqlite3_column_double(stmt, 7);
    
    inverter_params.L1 = sqlite3_column_double(stmt, 8);
    inverter_params.R1 = sqlite3_column_double(stmt, 9);
    inverter_params.C1 = sqlite3_column_double(stmt, 10);
    inverter_params.L2 = sqlite3_column_double(stmt, 11);
    inverter_params.R2 = sqlite3_column_double(stmt, 12);
    inverter_params.C2 = sqlite3_column_double(stmt, 13);
    
    inverter_params.Lboost = sqlite3_column_double(stmt, 14);
    inverter_params.RLboost = sqlite3_column_double(stmt, 15);
    inverter_params.Cboost = sqlite3_column_double(stmt, 16);
    inverter_params.boost_frequency = sqlite3_column_double(stmt, 17);
    
    sqlite3_finalize(stmt);
    return true;
}

bool ComponentDatabase::load_grid(const std::string& part_number,
                                  GridParameters& grid_params) {
    if (!db_) return false;
    
    const char* sql = "SELECT grid_voltage, grid_frequency, grid_phase, "
                     "reference_phase_magnitude, reference_frequency, reference_phase_shift "
                     "FROM grid WHERE part_number = ?";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, part_number.c_str(), -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }
    
    grid_params.part_number = part_number;
    grid_params.grid_voltage = sqlite3_column_double(stmt, 0);
    grid_params.grid_frequency = sqlite3_column_double(stmt, 1);
    grid_params.grid_phase = sqlite3_column_double(stmt, 2);
    grid_params.reference_phase_magnitude = sqlite3_column_double(stmt, 3);
    grid_params.reference_frequency = sqlite3_column_double(stmt, 4);
    grid_params.reference_phase_shift = sqlite3_column_double(stmt, 5);
    
    sqlite3_finalize(stmt);
    return true;
}

