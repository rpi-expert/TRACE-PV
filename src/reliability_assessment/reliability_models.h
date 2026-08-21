#ifndef RELIABILITY_MODELS_H
#define RELIABILITY_MODELS_H

#include <vector>
#include <string>

// Capacitor types
enum class CapacitorType {
    ALUMINUM_ELECTROLYTIC,
    TANTALUM,
    CERAMIC,
    FILM
};

// Film capacitor coefficients structure
// Lifetime = A * (RH_op)^-n * exp((E_a / k_B) * (1 / T_op)) * exp(β * V_op)
struct FilmCapacitorCoefficients {
    double A;           // Base coefficient
    double n;           // Humidity exponent
    double Ea;          // Activation energy (eV)
    double beta;        // Voltage coefficient
    double V_rated;     // Rated voltage (V)
    double T_ref;       // Reference temperature (K)
    double RH_ref;      // Reference relative humidity (%)
};

// Aluminum electrolytic capacitor coefficients structure
// Lifetime = L_0 * (V / V_0)^-β_1 * 2^((T_0 - T) / 10)
struct AluminumElectrolyticCoefficients {
    double L0;          // Base lifetime (hours)
    double V0;          // Reference voltage (V)
    double T0;          // Reference temperature (°C)
    double beta_min;    // Minimum voltage stress exponent
    double beta_max;    // Maximum voltage stress exponent
};

// Capacitor coefficients structure (union to hold either type)
struct CapacitorCoefficients {
    CapacitorType type;
    union {
        FilmCapacitorCoefficients film;
        AluminumElectrolyticCoefficients aluminum;
    } coeffs;
    // Thermal and electrical parameters
    double rth_amb;      // Thermal resistance from ambient to hotspot (K/W)
    double rth_surf;     // Thermal resistance from ambient to surface (K/W)
    double esr;          // Equivalent series resistance (Ohm)
    double capacitance;  // Capacitance (Farads)
};

// Power module deltaT model coefficients structure
// Nf = A * ΔT^(n) * e^(Ea / (K_B * T_max))
struct PowerModuleDeltaTCoefficients {
    double A;           // Base coefficient
    double n;           // Temperature range exponent
    double Ea;          // Activation energy (eV)
};

// Power module Arrhenius model coefficients structure
// Lifetime = Lifetime_stress * (RH_stress / RH_op)^n * exp[ (Ea / K_B) * (1 / T_op - 1 / T_stress) ] * (Voltage_stress / Voltage_op)^(n_2)
// For IGBT, simplified to: Lifetime = A * exp[ (Ea / K_B) * (1 / T_op - 1 / T_ref) ] * T_op^n1 * (other factors)^n2
// Simplified version: Lifetime = A * T_op^n1 * exp[ (Ea / K_B) / T_op ]
struct PowerModuleArrheniusCoefficients {
    double A;           // Base coefficient
    double n1;          // Temperature exponent
    double n2;          // Additional exponent (for future use)
    double Ea;          // Activation energy (eV)
    double RH_ref;      // Reference relative humidity (%)
    double T_ref;       // Reference temperature (K)
    double V_ref;       // Reference voltage (V)
};

// Power module coefficients structure (union to hold both models)
struct PowerModuleCoefficients {
    PowerModuleDeltaTCoefficients deltaT_model;
    PowerModuleArrheniusCoefficients arrhenius_model;
};

// PCB ductility coefficients structure
struct PCBDuctilityCoefficients {
    double A;
    double B;
    double C;
    double D;
    double E;
};

// PCB component dimensions structure
struct PCBComponentDimensions {
    double length;      // Component length in mm
    double width;       // Component width in mm
    double thickness;   // Component thickness in mm
};

// PCB copper pad dimensions structure
struct PCBCopperDimensions {
    double length;      // Copper pad length in mm
    double width;       // Copper pad width in mm
    double thickness;   // Copper pad thickness in mm
};

// PCB solder joint dimensions structure
struct PCBSolderDimensions {
    double length;      // Solder joint length in mm
    double width;       // Solder joint width in mm
    double thickness;   // Solder joint thickness in mm
};

// Complete PCB parameters structure
struct PCBParameters {
    std::string component_type;  // e.g., "MELF", "DE"
    std::string material;        // e.g., "Leadless SAC"
    
    // Component dimensions
    PCBComponentDimensions component_dims;
    
    // Copper pad dimensions
    PCBCopperDimensions copper_dims;
    
    // Solder joint dimensions
    PCBSolderDimensions solder_dims;
    
    // Ductility coefficients
    PCBDuctilityCoefficients ductility;
    
    // Material properties
    double shear_modulus;        // G_solder (Pa)
    double CTE_component;        // Component CTE (1/K)
    double G_copper;             // Copper shear modulus (Pa)
    double G_FR4;                // FR4 shear modulus (Pa)
    double CTE_FR4;              // FR4 CTE (1/K)
    double E_FR4;                // FR4 elastic modulus (Pa)
    double Poisson_FR4;          // FR4 Poisson ratio
    double pcb_thickness;        // PCB thickness (mm)
    
    // Thermal cycling parameters
    double max_temperature;      // Max temperature (°C)
    double min_temperature;       // Min temperature (°C)
    double dwell_time;            // Dwell time (s)
    double ramp_time;             // Ramp time (s)
    
    // Lifetime parameters
    double beta;                 // Beta parameter
    double lifetime_N63;          // Lifetime at N63
    double adjust_param;          // Adjustment parameter for model calibration
};

// Fan electrical coefficients structure
// Lifetime_electrical = A * RH_op^n * exp(-E_a / (k_B * T_op))
struct FanElectricalCoefficients {
    double A;           // Base coefficient
    double n;           // Humidity exponent
    double Ea;          // Activation energy (eV)
};

// Fan mechanical coefficients structure
// Lifetime_mechanical = A * exp(c * T_op)
struct FanMechanicalCoefficients {
    double A;           // Base coefficient
    double c;           // Temperature coefficient
};

// Fan coefficients structure (union to hold both types)
struct FanCoefficients {
    FanElectricalCoefficients electrical;
    FanMechanicalCoefficients mechanical;
};

// Rainflow counting result structure
struct RainflowResult {
    std::vector<double> delta_range;    // Temperature range for each cycle
    std::vector<double> delta_cycle;    // Number of cycles
    std::vector<size_t> begin_idx;      // Begin index for each cycle
    std::vector<size_t> end_idx;        // End index for each cycle
    std::vector<bool> is_full_cycle;    // true for full cycle, false for half cycle
    double Tj_max;                      // Maximum junction temperature
};

// Default coefficients (to be defined in .cpp file)
extern const CapacitorCoefficients DEFAULT_CAPACITOR_COEFFS;
extern const PowerModuleCoefficients DEFAULT_POWER_MODULE_COEFFS;
extern const FanCoefficients DEFAULT_FAN_COEFFS;

// PV Inverter parameters structure
struct PVInverterParameters {
    std::string part_number;
    bool boost_included;
    int model_stage;
    int topology_level;
    std::string modulation_scheme;
    double switching_frequency;  // Hz
    double Vdc;                   // V
    double CDC1;                  // F (three-level upper)
    double CDC2;                  // F (three-level lower)
    double L1;                    // H
    double R1;                    // Ohm
    double C1;                    // F
    double L2;                    // H
    double R2;                    // Ohm
    double C2;                    // F
    double Lboost;                // H
    double RLboost;               // Ohm
    double Cboost;                // F
    double boost_frequency;        // Hz
};

// Grid parameters structure
struct GridParameters {
    std::string part_number;
    double grid_voltage;              // V
    double grid_frequency;            // Hz
    double grid_phase;                // rad
    double reference_phase_magnitude;  // V
    double reference_frequency;       // Hz
    double reference_phase_shift;      // rad
};

// Physical constants
const double BOLTZMANN_CONSTANT = 8.617333262e-5;  // eV/K
const double ABSOLUTE_ZERO = 273.15;               // Celsius to Kelvin offset

#endif // RELIABILITY_MODELS_H

