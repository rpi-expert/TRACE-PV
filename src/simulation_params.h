#pragma once

#include <string>
#include <vector>

enum class ModulationType {
    SVM,
    SPWM
};

constexpr double PI = 3.14159265358979323846;

struct SimulationParameters {
    // Note: topology_level and model_stage are retrieved from component_profile (PV inverter profile)
    // They are set automatically when using create_parameters_from_database()
    int topology_level;   // 2 or 3 (from component_profile)
    int model_stage;      // 1 or 2 (from component_profile)
    ModulationType modulation;

    double simulation_time;        // seconds
    double switching_frequency;    // Hz
    double switching_period;       // seconds
    int avg_points_per_period;     // samples per switching period (average model)
    int a2s_points_per_interval;   // samples per switching interval (A2S model)

    // Electrical quantities
    double v_pv;                   // PV/DC source voltage (V)
    double vdc_target;             // Target DC-link voltage (V)
    double boost_duty;             // Boost duty cycle (stage 2)

    double vg_mag;                 // Grid peak phase voltage (V)
    double vg_freq;                // Grid frequency (Hz)
    double vg_phase;               // Grid phase (rad)

    double reference_phase_magnitude; // reference phase voltage magnitude (V)
    double reference_frequency;       // Hz
    double reference_phase_shift;     // rad

    // LCL filter parameters
    double L1;   // H
    double RL1;  // Ohm
    double C;    // F
    double RC;   // Ohm
    double L2;   // H
    double RL2;  // Ohm

    // Boost stage parameters
    double Lboost;   // H
    double RLboost;  // Ohm
    double CDC;      // F (single dc link)
    double CDC1;     // F (three-level upper)
    double CDC2;     // F (three-level lower)

    int num_states;                // state vector size (avg/A2S)
    int num_steps;                 // total samples for average model output
};

SimulationParameters create_default_parameters(int topology_level,
                                               int model_stage,
                                               ModulationType modulation);

// Create simulation parameters from database (loads from pv_inverter and grid profiles)
SimulationParameters create_parameters_from_database(
    class ComponentDatabase& component_db,
    const std::string& pv_inverter_part_number,
    const std::string& grid_part_number,
    ModulationType modulation);
void override_simulation_time(SimulationParameters& params, double new_time_seconds);
std::vector<double> generate_time_vector(const SimulationParameters& params);
std::vector<double> compute_input_voltage(const SimulationParameters& params,
                                          const std::vector<double>& time_points);
std::string make_waveform_filename(const SimulationParameters& params);

ModulationType parse_modulation(const std::string& name);
std::string modulation_to_string(ModulationType modulation);
