#include "simulation_params.h"
#include "component_database/component_database.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
constexpr double BASE_AVG_POINTS = 10.0;
constexpr double BASE_A2S_POINTS = 10.0;

void recalculate_num_steps(SimulationParameters& params) {
    const double min_time = std::max(params.switching_period, 1e-9);
    const double sim_time = std::max(params.simulation_time, min_time);
    const int periods = static_cast<int>(std::ceil(sim_time / params.switching_period));
    params.num_steps = std::max(1, periods * params.avg_points_per_period);
}

SimulationParameters initialise_defaults(int topology_level,
                                         int model_stage,
                                         ModulationType modulation) {
    if (topology_level != 2 && topology_level != 3) {
        throw std::invalid_argument("topology_level must be 2 or 3");
    }
    if (model_stage != 1 && model_stage != 2) {
        throw std::invalid_argument("model_stage must be 1 or 2");
    }

    SimulationParameters params{};
    params.topology_level = topology_level;
    params.model_stage = model_stage;
    params.modulation = modulation;

    // Set simulation time to one fundamental cycle (60 Hz = 1/60 seconds)
    constexpr double FUNDAMENTAL_FREQ = 60.0;
    params.simulation_time = 1.0 / FUNDAMENTAL_FREQ;
    
    // Default values for parameters not in profiles (will be overridden if loading from database)
    params.switching_frequency = 16000.0; // 16 kHz across MATLAB scripts
    params.switching_period = 1.0 / params.switching_frequency;
    params.avg_points_per_period = static_cast<int>(BASE_AVG_POINTS);
    params.a2s_points_per_interval = static_cast<int>(BASE_A2S_POINTS);

    params.v_pv = 1000.0;  // Default, should come from PV panel profile in future
    params.vdc_target = 1000.0;  // Default
    params.boost_duty = (model_stage == 2) ? (1.0 - params.v_pv / params.vdc_target) : 0.0;

    params.vg_mag = 480.0 * std::sqrt(2.0) / std::sqrt(3.0);
    params.vg_freq = 60.0;
    params.vg_phase = 0.0;

    params.reference_phase_magnitude = 285.7229 * std::sqrt(2.0);
    params.reference_frequency = 60.0;
    params.reference_phase_shift = 0.1974; // radians

    params.L1 = 3.0e-4;
    params.RL1 = 0.03;
    params.C = 9.7860e-05;
    params.RC = 1.0;  // Not in profile, keep default
    params.L2 = 9.3183e-04;
    params.RL2 = 0.0053;

    params.Lboost = (model_stage == 2) ? 5.0e-4 : 0.0;
    params.RLboost = (model_stage == 2) ? 0.005 : 0.0;
    params.CDC = (model_stage == 2 && topology_level == 2) ? 10351e-6 : 0.0;
    params.CDC1 = (model_stage == 2 && topology_level == 3) ? 10351e-6 : 0.0;
    params.CDC2 = (model_stage == 2 && topology_level == 3) ? 10351e-6 : 0.0;

    if (topology_level == 2 && model_stage == 1) {
        params.num_states = 9;
    } else if (topology_level == 2 && model_stage == 2) {
        params.num_states = 11;
    } else if (topology_level == 3 && model_stage == 1) {
        params.num_states = 9;
    } else if (topology_level == 3 && model_stage == 2) {
        params.num_states = 12;
    }

    recalculate_num_steps(params);

    return params;
}

} // namespace

SimulationParameters create_default_parameters(int topology_level,
                                               int model_stage,
                                               ModulationType modulation) {
    return initialise_defaults(topology_level, model_stage, modulation);
}

// Create simulation parameters from database (loads from pv_inverter and grid profiles)
SimulationParameters create_parameters_from_database(
    ComponentDatabase& component_db,
    const std::string& pv_inverter_part_number,
    const std::string& grid_part_number,
    ModulationType modulation) {
    
    SimulationParameters params{};
    
    // Load PV inverter parameters
    PVInverterParameters inverter_params;
    if (!component_db.load_pv_inverter(pv_inverter_part_number, inverter_params)) {
        throw std::runtime_error("Failed to load PV inverter: " + pv_inverter_part_number);
    }
    
    // Load grid parameters
    GridParameters grid_params;
    if (!component_db.load_grid(grid_part_number, grid_params)) {
        throw std::runtime_error("Failed to load grid: " + grid_part_number);
    }
    
    // Set parameters from inverter profile
    params.topology_level = inverter_params.topology_level;
    params.model_stage = inverter_params.model_stage;
    params.modulation = modulation;  // Use provided modulation (may override profile)
    params.switching_frequency = inverter_params.switching_frequency;
    params.switching_period = 1.0 / params.switching_frequency;
    params.vdc_target = inverter_params.Vdc;
    
    // LCL filter parameters from inverter profile
    params.L1 = inverter_params.L1;
    params.RL1 = inverter_params.R1;
    params.C = inverter_params.C1;  // Using C1 as C (main filter capacitor)
    params.L2 = inverter_params.L2;
    params.RL2 = inverter_params.R2;
    
    // Boost parameters from inverter profile
    params.Lboost = inverter_params.Lboost;
    params.RLboost = inverter_params.RLboost;
    params.CDC1 = inverter_params.CDC1;
    params.CDC2 = inverter_params.CDC2;
    
    // Calculate CDC for 2-level (not in profile, calculate from CDC1 if available)
    if (params.topology_level == 2 && params.model_stage == 2) {
        params.CDC = (inverter_params.CDC1 > 0.0) ? inverter_params.CDC1 : 10351e-6;
    } else {
        params.CDC = 0.0;
    }
    
    // Set parameters from grid profile
    params.vg_mag = grid_params.grid_voltage;
    params.vg_freq = grid_params.grid_frequency;
    params.vg_phase = grid_params.grid_phase;
    params.reference_phase_magnitude = grid_params.reference_phase_magnitude;
    params.reference_frequency = grid_params.reference_frequency;
    params.reference_phase_shift = grid_params.reference_phase_shift;
    
    // Parameters not in profiles (keep defaults)
    constexpr double FUNDAMENTAL_FREQ = 60.0;
    params.simulation_time = 1.0 / FUNDAMENTAL_FREQ;
    params.avg_points_per_period = static_cast<int>(BASE_AVG_POINTS);
    params.a2s_points_per_interval = static_cast<int>(BASE_A2S_POINTS);
    params.v_pv = 1000.0;  // Default, should come from PV panel profile in future
    params.boost_duty = (params.model_stage == 2) ? (1.0 - params.v_pv / params.vdc_target) : 0.0;
    params.RC = 1.0;  // Not in profile, keep default
    
    // Calculate num_states based on topology and stage
    if (params.topology_level == 2 && params.model_stage == 1) {
        params.num_states = 9;
    } else if (params.topology_level == 2 && params.model_stage == 2) {
        params.num_states = 11;
    } else if (params.topology_level == 3 && params.model_stage == 1) {
        params.num_states = 9;
    } else if (params.topology_level == 3 && params.model_stage == 2) {
        params.num_states = 12;
    }
    
    recalculate_num_steps(params);
    
    return params;
}

void override_simulation_time(SimulationParameters& params, double new_time_seconds) {
    if (new_time_seconds <= 0.0) {
        throw std::invalid_argument("Simulation time must be positive");
    }
    params.simulation_time = new_time_seconds;
    recalculate_num_steps(params);
}

std::vector<double> generate_time_vector(const SimulationParameters& params) {
    std::vector<double> time_points;
    time_points.reserve(params.num_steps);

    const double dt = params.switching_period / params.avg_points_per_period;
    for (int i = 0; i < params.num_steps; ++i) {
        double t = i * dt;
        if (t > params.simulation_time) {
            t = params.simulation_time;
        }
        time_points.push_back(t);
    }
    return time_points;
}

std::vector<double> compute_input_voltage(const SimulationParameters& params,
                                          const std::vector<double>& time_points) {
    std::vector<double> waveform;
    waveform.reserve(time_points.size());

    const double base_dc = (params.model_stage == 1) ? params.v_pv : params.vdc_target;

    double level_scale = 0.0;
    double switching_gain = 0.0;
    double harmonic_gain = 0.0;

    switch (params.modulation) {
        case ModulationType::SVM:
            level_scale = (params.topology_level == 2) ? 0.87 : 1.20;
            switching_gain = (params.topology_level == 2) ? 0.18 : 0.25;
            harmonic_gain = 0.066;
            break;
        case ModulationType::SPWM:
            level_scale = (params.topology_level == 2) ? 0.78 : 0.95;
            switching_gain = (params.topology_level == 2) ? 0.12 : 0.18;
            harmonic_gain = 0.0;
            break;
        default:
            throw std::runtime_error("Unsupported modulation type");
    }

    for (double t : time_points) {
        const double grid_component = std::sin(2.0 * PI * params.reference_frequency * t + params.reference_phase_shift);
        const double switching_component = std::sin(2.0 * PI * params.switching_frequency * t);
        const double third_harmonic = std::sin(6.0 * PI * params.reference_frequency * t + params.reference_phase_shift);

        const double modulated_voltage = base_dc * level_scale * grid_component;
        const double hf_component = base_dc * switching_gain * switching_component;
        const double harmonic_injection = harmonic_gain * base_dc * third_harmonic;

        waveform.push_back(modulated_voltage + hf_component + harmonic_injection);
    }

    return waveform;
}

std::string make_waveform_filename(const SimulationParameters& params) {
    return std::to_string(params.topology_level) + "level_" +
           std::to_string(params.model_stage) + "stage_" +
           modulation_to_string(params.modulation) + "_i1.csv";
}

ModulationType parse_modulation(const std::string& name) {
    if (name == "svm" || name == "SVM" || name == "regular") {
        return ModulationType::SVM;
    }
    if (name == "spwm" || name == "SPWM") {
        return ModulationType::SPWM;
    }
    throw std::invalid_argument("Unknown modulation type: " + name);
}

std::string modulation_to_string(ModulationType modulation) {
    switch (modulation) {
        case ModulationType::SVM:
            return "svm";
        case ModulationType::SPWM:
            return "spwm";
    }
    return "unknown";
}
