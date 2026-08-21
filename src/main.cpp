#include "multi_physics_simulator/electrical_simulation/a2s_gpu.h"
#include "simulation_params.h"
#include "simulation_case.h"
#include "multi_physics_simulator/electrical_simulation/gpu_capacity.h"
#include "multi_physics_simulator/electrical_simulation/stress_calculation.h"
#include "multi_physics_simulator/thermal_simulation/capacitor_loss_thermal_model.h"
#include "multi_physics_simulator/thermal_simulation/igbt_loss_thermal_model.h"
#include "multi_physics_simulator/thermal_simulation/loss_model.h"
#include "multi_physics_simulator/thermal_simulation/thermal_model.h"
#include "multi_physics_simulator/environmental_simulation/internal_conditions.h"
#include "component_database/component_database.h"
// IVCurveSimulator converted to Python - commented out for now
// #include "component_database/offline_trainning/iv_curve_simulator.h"
#include "simulation_model.h"
#include "simulation_preparation/pv_voltage_iv_curve.h"
#include "simulation_preparation/mission_profile_loader.h"
#include "reliability_assessment/reliability_models.h"
#include "reliability_assessment/rainflow_counting.h"
#include "reliability_assessment/fan_cooling_reliability.h"
#include "reliability_assessment/igbt_reliability.h"
#include "reliability_assessment/capacitor_reliability.h"
#include "reliability_assessment/pcb_reliability.h"
#include "reliability_assessment/reliability_kernels.h"
#include "reliability_assessment/reliability_kernels.h"
#include <algorithm>
#include <array>
#include <map>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <cmath>
#include <omp.h>
#include <cuda_runtime.h>
#ifdef __linux__
#include <malloc.h>
#endif

namespace {

constexpr int kStaticCasesPerYear5Min = 365 * 24 * 12;
constexpr double kCriticalDegradationStep = 0.10;

struct ProfilingTotals {
    double static_cache_electrical = 0.0;
    double electrical_gpu = 0.0;
    double stress_loss_thermal = 0.0;
    double stress_calculation = 0.0;
    double capacitor_loss = 0.0;
    double power_module_loss = 0.0;
    double capacitor_thermal = 0.0;
    double capacitor_ref_harmonic = 0.0;
    double capacitor_ref_esr_grid = 0.0;
    double capacitor_ref_loss_grid = 0.0;
    double capacitor_ref_polyfit = 0.0;
    double capacitor_ref_iteration = 0.0;
    double capacitor_fallback_static = 0.0;
    double igbt_thermal = 0.0;
    double igbt_parameter_lookup = 0.0;
    double igbt_input_build = 0.0;
    double igbt_loss_igbt1 = 0.0;
    double igbt_loss_igbt2 = 0.0;
    double igbt_loss_diode1 = 0.0;
    double igbt_loss_diode2 = 0.0;
    double igbt_thermal_rc = 0.0;
    double fan_reliability = 0.0;
    double capacitor_reliability = 0.0;
    double igbt_reliability = 0.0;
    double pcb_reliability = 0.0;
    double stressor_csv = 0.0;
};

struct CaseStressorRecord {
    int case_index = 0;
    double fan_electrical_external = 0.0;
    double fan_electrical_internal = 0.0;
    double fan_mechanical_external = 0.0;
    double fan_mechanical_internal = 0.0;
    double capacitor = 0.0;
    double igbt_deltaT = 0.0;
    double igbt_arrhenius = 0.0;
    double pcb = 0.0;
};

struct RoundStressorTotals {
    double fan_electrical_external = 0.0;
    double fan_electrical_internal = 0.0;
    double fan_mechanical_external = 0.0;
    double fan_mechanical_internal = 0.0;
    double capacitor = 0.0;
    double igbt_deltaT = 0.0;
    double igbt_arrhenius = 0.0;
    double pcb = 0.0;
};

using DegradationParameterState = std::array<int, 8>;
using DegradationValues = std::array<double, 8>;

int days_in_calendar_month(int year, int month) {
    static constexpr std::array<int, 12> kDays = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    int days = kDays[static_cast<std::size_t>(month - 1)];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) {
        ++days;
    }
    return days;
}

bool parse_calendar_month_coordinate(const std::string& timestamp, double& coordinate) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(timestamp.c_str(), "%d-%d-%d %d:%d:%d",
                    &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }
    const int days_in_month = days_in_calendar_month(year, month);
    if (days_in_month <= 0 || day < 1 || day > days_in_month ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return false;
    }
    const double day_fraction =
        (static_cast<double>(day - 1) +
         (static_cast<double>(hour) +
          (static_cast<double>(minute) + static_cast<double>(second) / 60.0) / 60.0) /
             24.0) /
        static_cast<double>(days_in_month);
    coordinate = static_cast<double>(year * 12 + (month - 1)) + day_fraction;
    return true;
}

struct RoundComputationCache {
    bool valid = false;
    DegradationParameterState parameter_state{};
    RoundStressorTotals totals;
    std::vector<CaseStressorRecord> records;
    int cases_processed = 0;
};

template <typename T>
void clear_vector_storage(std::vector<T>& values) {
    std::vector<T>().swap(values);
}

void write_double_vector(std::ostream& out, const std::vector<double>& values) {
    const std::uint64_t size = static_cast<std::uint64_t>(values.size());
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    if (!values.empty()) {
        out.write(reinterpret_cast<const char*>(values.data()),
                  static_cast<std::streamsize>(values.size() * sizeof(double)));
    }
}

bool read_double_vector(std::istream& in, std::vector<double>& values) {
    std::uint64_t size = 0;
    in.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (!in) {
        return false;
    }
    values.resize(static_cast<std::size_t>(size));
    if (!values.empty()) {
        in.read(reinterpret_cast<char*>(values.data()),
                static_cast<std::streamsize>(values.size() * sizeof(double)));
    }
    return static_cast<bool>(in);
}

void write_igbt_time_value(std::ostream& out, const IgbtGpuTimeValue& tv) {
    write_double_vector(out, tv.t);
    write_double_vector(out, tv.y);
}

bool read_igbt_time_value(std::istream& in, IgbtGpuTimeValue& tv) {
    return read_double_vector(in, tv.t) && read_double_vector(in, tv.y);
}

void write_igbt_device_input(std::ostream& out, const IgbtGpuDeviceInput& device) {
    write_igbt_time_value(out, device.fundamental_i);
    write_igbt_time_value(out, device.rising_i);
    write_igbt_time_value(out, device.falling_v);
    write_igbt_time_value(out, device.falling_i);
    write_igbt_time_value(out, device.rising_v);
}

bool read_igbt_device_input(std::istream& in, IgbtGpuDeviceInput& device) {
    return read_igbt_time_value(in, device.fundamental_i) &&
           read_igbt_time_value(in, device.rising_i) &&
           read_igbt_time_value(in, device.falling_v) &&
           read_igbt_time_value(in, device.falling_i) &&
           read_igbt_time_value(in, device.rising_v);
}

struct CliOptions {
    std::string topology;  // "2l2s", "2l1s", "3l2s", "3l1s"
    std::string simulation_model_file = "simulator_inputs/simulation_model/example_simulation_model.json";
    std::string input_mode = "mission";  // "mission" or "static"
    std::string mission_csv_file;
    std::string environmental_csv_file = "simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv";
    std::string operating_csv_file = "simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv";
    double static_temperature = 95.0;
    double static_rh = 95.0;
    double static_voltage = 500.0;
    double static_power = 300.0;
    double static_irradiance = 1000.0;
    int static_cases = kStaticCasesPerYear5Min;
    int num_rounds = 1;
    int max_iterations = 0;  // 0 means run until degradation reaches 1.0
    std::string post_processing_mode = "reference";  // fast, reference, or hybrid
    double thermal_step_s = 0.0;  // 0 means use switching timestep
    ComputePrecision precision = ComputePrecision::Double;
    bool pipeline_enabled = true;
    int batch_size_limit = 0;  // 0 means use calculated GPU capacity
    ModulationType modulation = ModulationType::SVM;
    int num_gpus = -1;  // -1 means use all available GPUs
};

std::string precision_to_string(ComputePrecision precision) {
    return precision == ComputePrecision::Float ? "float" : "double";
}

ComputePrecision parse_precision(const std::string& name) {
    if (name == "double" || name == "DOUBLE") {
        return ComputePrecision::Double;
    }
    if (name == "float" || name == "single" || name == "FLOAT" || name == "SINGLE") {
        return ComputePrecision::Float;
    }
    throw std::invalid_argument("--precision must be double or float");
}

bool parse_on_off(const std::string& value, const std::string& option_name) {
    if (value == "on" || value == "ON" || value == "true" || value == "TRUE" || value == "1") {
        return true;
    }
    if (value == "off" || value == "OFF" || value == "false" || value == "FALSE" || value == "0") {
        return false;
    }
    throw std::invalid_argument(option_name + " must be on/off, true/false, or 1/0");
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " --topology <2l2s|2l1s|3l2s|3l1s> [OPTIONS]" << std::endl;
    std::cout << "  --topology: Topology type:" << std::endl;
    std::cout << "     2l2s = two-level two-stage" << std::endl;
    std::cout << "     2l1s = two-level single-stage" << std::endl;
    std::cout << "     3l2s = three-level two-stage" << std::endl;
    std::cout << "     3l1s = three-level single-stage" << std::endl;
    std::cout << "  --input-mode: Input mode, mission or static (default: mission)" << std::endl;
    std::cout << "  --mission-csv / --csv: Combined mission profile CSV" << std::endl;
    std::cout << "  --environmental-csv: Environmental mission profile CSV for split-file mode" << std::endl;
    std::cout << "  --operating-csv: Operating mission profile CSV for split-file mode" << std::endl;
    std::cout << "  --static-temp: Static ambient temperature in Celsius (default: 95)" << std::endl;
    std::cout << "  --static-rh: Static relative humidity in percent (default: 95)" << std::endl;
    std::cout << "  --static-voltage: Static AC voltage RMS line-to-line in volts (default: 500)" << std::endl;
    std::cout << "  --static-power: Static AC power in watts for thermal model (default: 300)" << std::endl;
    std::cout << "  --static-irradiance: Static solar irradiance in W/m^2 (default: 1000)" << std::endl;
    std::cout << "  --static-cases: Number of repeated 5-minute static cases (default: 105120 = one year)" << std::endl;
    std::cout << "  --rounds: Number of chunks used to process one mission/static profile (default: 1)" << std::endl;
    std::cout << "  --max-iterations: Maximum times to repeat the whole mission/static profile before stopping (default: 0, unlimited)" << std::endl;
    std::cout << "  --post-processing-mode: fast, reference, or hybrid (default: reference)" << std::endl;
    std::cout << "     fast = fallback loss/thermal + per-case degradation, skips reference thermal/rainflow" << std::endl;
    std::cout << "     reference = detailed reference thermal and cycle models" << std::endl;
    std::cout << "     hybrid = currently aliases reference; reserved for sampled calibration" << std::endl;
    std::cout << "  --thermal-step: Reference IGBT thermal integration step in seconds (default: 0 = switching timestep)" << std::endl;
    std::cout << "  --precision: double or float for electrical waveform output precision (default: double)" << std::endl;
    std::cout << "  --pipeline: on/off, overlap next electrical GPU batch with current CPU post-processing (default: on)" << std::endl;
    std::cout << "  --batch-size-limit: Optional maximum cases per electrical batch (default: 0 = auto)" << std::endl;
    std::cout << "  --modulation: Modulation type svm or spwm (default: svm)" << std::endl;
    std::cout << "  --ngpus: Number of GPUs to use, or 'all' to use all available (default: use all available)" << std::endl;
    std::cout << "  --model: Simulation model JSON file with component part numbers (default: simulator_inputs/simulation_model/example_simulation_model.json)" << std::endl;
    std::cout << "\nMission mode loads --mission-csv when supplied, otherwise split files:" << std::endl;
    std::cout << "  - simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv" << std::endl;
    std::cout << "  - simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv" << std::endl;
}

CliOptions parse_arguments(int argc, char** argv) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--topology" || arg == "-t") && (i + 1) < argc) {
            opts.topology = argv[++i];
        } else if ((arg == "--rounds" || arg == "-r") && (i + 1) < argc) {
            opts.num_rounds = std::stoi(argv[++i]);
            if (opts.num_rounds <= 0) {
                throw std::invalid_argument("--rounds must be greater than 0");
            }
        } else if (arg == "--max-iterations" && (i + 1) < argc) {
            opts.max_iterations = std::stoi(argv[++i]);
            if (opts.max_iterations < 0) {
                throw std::invalid_argument("--max-iterations must be greater than or equal to 0");
            }
        } else if (arg == "--post-processing-mode" && (i + 1) < argc) {
            opts.post_processing_mode = argv[++i];
            if (opts.post_processing_mode != "fast" &&
                opts.post_processing_mode != "reference" &&
                opts.post_processing_mode != "hybrid") {
                throw std::invalid_argument("--post-processing-mode must be fast, reference, or hybrid");
            }
        } else if (arg == "--thermal-step" && (i + 1) < argc) {
            opts.thermal_step_s = std::stod(argv[++i]);
            if (opts.thermal_step_s < 0.0) {
                throw std::invalid_argument("--thermal-step must be greater than or equal to 0");
            }
        } else if (arg == "--precision" && (i + 1) < argc) {
            opts.precision = parse_precision(argv[++i]);
        } else if (arg == "--pipeline" && (i + 1) < argc) {
            opts.pipeline_enabled = parse_on_off(argv[++i], "--pipeline");
        } else if (arg == "--batch-size-limit" && (i + 1) < argc) {
            opts.batch_size_limit = std::stoi(argv[++i]);
            if (opts.batch_size_limit < 0) {
                throw std::invalid_argument("--batch-size-limit must be greater than or equal to 0");
            }
        } else if ((arg == "--modulation" || arg == "-m") && (i + 1) < argc) {
            opts.modulation = parse_modulation(argv[++i]);
        } else if ((arg == "--ngpus" || arg == "-g") && (i + 1) < argc) {
            std::string ngpus_arg = argv[++i];
            if (ngpus_arg == "all" || ngpus_arg == "ALL") {
                opts.num_gpus = -1;  // -1 means use all available GPUs
            } else {
                try {
                    opts.num_gpus = std::stoi(ngpus_arg);
                    if (opts.num_gpus <= 0) {
                        throw std::invalid_argument("--ngpus must be greater than 0 or 'all'");
                    }
                } catch (const std::exception& e) {
                    throw std::invalid_argument("--ngpus must be a positive integer or 'all', got: " + ngpus_arg);
                }
            }
        } else if ((arg == "--model" || arg == "-M") && (i + 1) < argc) {
            opts.simulation_model_file = argv[++i];
        } else if (arg == "--input-mode" && (i + 1) < argc) {
            opts.input_mode = argv[++i];
            if (opts.input_mode != "mission" && opts.input_mode != "static") {
                throw std::invalid_argument("--input-mode must be 'mission' or 'static'");
            }
        } else if ((arg == "--mission-csv" || arg == "--csv" || arg == "-c") && (i + 1) < argc) {
            opts.mission_csv_file = argv[++i];
            opts.input_mode = "mission";
        } else if (arg == "--environmental-csv" && (i + 1) < argc) {
            opts.environmental_csv_file = argv[++i];
            opts.input_mode = "mission";
        } else if (arg == "--operating-csv" && (i + 1) < argc) {
            opts.operating_csv_file = argv[++i];
            opts.input_mode = "mission";
        } else if (arg == "--static-temp" && (i + 1) < argc) {
            opts.static_temperature = std::stod(argv[++i]);
            opts.input_mode = "static";
        } else if (arg == "--static-rh" && (i + 1) < argc) {
            opts.static_rh = std::stod(argv[++i]);
            opts.input_mode = "static";
        } else if (arg == "--static-voltage" && (i + 1) < argc) {
            opts.static_voltage = std::stod(argv[++i]);
            opts.input_mode = "static";
        } else if (arg == "--static-power" && (i + 1) < argc) {
            opts.static_power = std::stod(argv[++i]);
            opts.input_mode = "static";
        } else if (arg == "--static-irradiance" && (i + 1) < argc) {
            opts.static_irradiance = std::stod(argv[++i]);
            opts.input_mode = "static";
        } else if (arg == "--static-cases" && (i + 1) < argc) {
            opts.static_cases = std::stoi(argv[++i]);
            opts.input_mode = "static";
            if (opts.static_cases <= 0) {
                throw std::invalid_argument("--static-cases must be greater than 0");
            }
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            std::exit(1);
        }
    }

    if (opts.topology.empty()) {
        throw std::invalid_argument("--topology is required");
    }
    if (opts.topology != "2l2s" && opts.topology != "2l1s" && 
        opts.topology != "3l2s" && opts.topology != "3l1s") {
        throw std::invalid_argument("--topology must be one of: 2l2s, 2l1s, 3l2s, 3l1s");
    }

    return opts;
}

// Topology selection removed - values now come from database profiles

// Calculate fundamental cycle size (samples per fundamental cycle)
int calculate_fundamental_cycle_size(const SimulationParameters& params) {
    const double fundamental_freq = params.vg_freq;  // 60 Hz
    const double switching_freq = params.switching_frequency;  // 16000 Hz
    
    // Number of switching periods per fundamental cycle
    // switching_periods_per_fundamental = switching_freq / fundamental_freq
    // For 16000 Hz / 60 Hz = 266.67 switching periods per fundamental cycle
    const double switching_periods_per_fundamental = switching_freq / fundamental_freq;
    
    // For each switching period: intervals * samples_per_interval
    const int intervals_per_period = (params.model_stage == 1) ? 7 : 8;
    const int samples_per_interval = params.a2s_points_per_interval;
    const int samples_per_switching_period = intervals_per_period * samples_per_interval;
    
    // Samples per fundamental cycle = switching_periods * samples_per_switching_period
    // Example: (16000/60) * (8 * 10) = 266.67 * 80 = 21333.33
    const int samples_per_fundamental = static_cast<int>(
        std::ceil(switching_periods_per_fundamental * samples_per_switching_period));
    
    return samples_per_fundamental;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto total_start = std::chrono::steady_clock::now();
        
        // Parse command line arguments
        const auto options = parse_arguments(argc, argv);
        
        // ====================================================================
        // Section 1: Load Simulation Model (Component Part Numbers)
        // ====================================================================
        SimulationModel sim_model;
        
        if (!load_simulation_model(options.simulation_model_file, sim_model)) {
            std::cerr << "Warning: Failed to load simulation model from " << options.simulation_model_file << std::endl;
            std::cerr << "  Using default component part numbers." << std::endl;
        } else {
            std::cout << "Simulation Model Loaded:" << std::endl;
            std::cout << "  Capacitor: " << sim_model.capacitor_part_number << std::endl;
            std::cout << "  Power Module: " << sim_model.power_module_part_number << std::endl;
            std::cout << "  Fan Cooling: " << sim_model.fan_cooling_part_number << std::endl;
            std::cout << "  PCB: " << sim_model.pcb_part_number << std::endl;
            std::cout << "  PV Panel: " << sim_model.pv_panel_part_number << std::endl;
        }
        
        // ====================================================================
        // Section 2: Initialize Component Database and Load Parameters
        // ====================================================================
        std::string db_path = "component_database/component_parameters.db";
        
        // Initialize component database
        ComponentDatabase component_db;
        if (!component_db.initialize(db_path)) {
            throw std::runtime_error("Failed to initialize component database. Database path: " + db_path);
        }
        
        // Load component parameters from simulation model
        // ComponentDatabase provides methods to retrieve model parameters by part number:
        // - load_capacitor(part_number, coeffs, capacitor_type)
        // - load_fan_cooling(part_number, coeffs)
        // - load_power_module(part_number, coeffs)
        // - load_pcb(part_number, pcb_params)
        // These can be used during simulation to get component-specific parameters
        std::cout << "\nComponent model parameters can be retrieved from database:" << std::endl;
        
        if (!sim_model.capacitor_part_number.empty()) {
            CapacitorCoefficients cap_coeffs;
            CapacitorType cap_type;
            std::string cap_voltage_type;
            if (component_db.load_capacitor(sim_model.capacitor_part_number, cap_coeffs, cap_type, cap_voltage_type)) {
                cap_coeffs.type = cap_type;
                std::string type_str = (cap_type == CapacitorType::FILM) ? "FILM" :
                                      (cap_type == CapacitorType::ALUMINUM_ELECTROLYTIC) ? "ALUMINUM_ELECTROLYTIC" :
                                      (cap_type == CapacitorType::TANTALUM) ? "TANTALUM" :
                                      (cap_type == CapacitorType::CERAMIC) ? "CERAMIC" : "UNKNOWN";
                std::cout << "  ✓ Capacitor " << sim_model.capacitor_part_number 
                          << " loaded (type: " << type_str << ", voltage: " << cap_voltage_type << ")" << std::endl;
            } else {
                std::cerr << "  ✗ Warning: Capacitor " << sim_model.capacitor_part_number 
                          << " not found in database" << std::endl;
                std::cerr << "    Make sure to run: cd component_database && python3 init_database.py" << std::endl;
            }
        }
        
        if (!sim_model.power_module_part_number.empty()) {
            PowerModuleCoefficients pm_coeffs;
            if (component_db.load_power_module(sim_model.power_module_part_number, pm_coeffs)) {
                std::cout << "  ✓ Power Module " << sim_model.power_module_part_number 
                          << " loaded" << std::endl;
                std::cerr << "DEBUG: IGBT Power Module Coefficients:" << std::endl;
                std::cerr << "  DeltaT Model:" << std::endl;
                std::cerr << "    A: " << pm_coeffs.deltaT_model.A << std::endl;
                std::cerr << "    n: " << pm_coeffs.deltaT_model.n << std::endl;
                std::cerr << "    Ea: " << pm_coeffs.deltaT_model.Ea << " eV" << std::endl;
                std::cerr << "  Arrhenius Model:" << std::endl;
                std::cerr << "    A: " << pm_coeffs.arrhenius_model.A << std::endl;
                std::cerr << "    n1: " << pm_coeffs.arrhenius_model.n1 << std::endl;
                std::cerr << "    n2: " << pm_coeffs.arrhenius_model.n2 << std::endl;
                std::cerr << "    Ea: " << pm_coeffs.arrhenius_model.Ea << " eV" << std::endl;
                std::cerr << "    RH_ref: " << pm_coeffs.arrhenius_model.RH_ref << " %" << std::endl;
                std::cerr << "    T_ref: " << pm_coeffs.arrhenius_model.T_ref << " K" << std::endl;
                std::cerr << "    V_ref: " << pm_coeffs.arrhenius_model.V_ref << " V" << std::endl;
            } else {
                std::cerr << "  ✗ Warning: Power Module " << sim_model.power_module_part_number 
                          << " not found in database" << std::endl;
            }
        }
        
        if (!sim_model.fan_cooling_part_number.empty()) {
            FanCoefficients fan_coeffs;
            if (component_db.load_fan_cooling(sim_model.fan_cooling_part_number, fan_coeffs)) {
                std::cout << "  ✓ Fan Cooling " << sim_model.fan_cooling_part_number 
                          << " loaded" << std::endl;
            } else {
                std::cerr << "  ✗ Warning: Fan Cooling " << sim_model.fan_cooling_part_number 
                          << " not found in database" << std::endl;
            }
        }
        
        if (!sim_model.pcb_part_number.empty()) {
            PCBParameters pcb_params;
            if (component_db.load_pcb(sim_model.pcb_part_number, pcb_params)) {
                std::cout << "  ✓ PCB " << sim_model.pcb_part_number 
                          << " loaded" << std::endl;
            } else {
                std::cerr << "  ✗ Warning: PCB " << sim_model.pcb_part_number 
                          << " not found in database" << std::endl;
            }
        }
        
        // ====================================================================
        // Section 3: Initialize IV Curve Simulator for PV Panel
        // ====================================================================
        // IVCurveSimulator converted to Python - commented out for now
        // TODO: Create C++ wrapper or integrate Python version
        // std::unique_ptr<IVCurveSimulator> iv_simulator = nullptr;
        
        // if (!sim_model.pv_panel_part_number.empty()) {
        //     iv_simulator = std::make_unique<IVCurveSimulator>();
        //     if (iv_simulator->initialize(db_path, sim_model.pv_panel_part_number)) {
        //         std::cout << "\nIV Curve Simulator initialized for PV panel: " 
        //                   << sim_model.pv_panel_part_number << std::endl;
        //     } else {
        //         std::cerr << "Warning: Failed to initialize IV curve simulator for panel: " 
        //                   << sim_model.pv_panel_part_number << std::endl;
        //         iv_simulator.reset();
        //     }
        // }
        // ====================================================================
        
        std::vector<SimulationCase> all_cases;
        std::string input_description;

        if (options.input_mode == "static") {
            all_cases = create_static_mission_profile(
                options.static_temperature,
                options.static_rh,
                options.static_voltage,
                options.static_power,
                options.static_cases,
                options.static_irradiance
            );
            input_description = "static values";
        } else if (!options.mission_csv_file.empty()) {
            all_cases = load_mission_profile_csv(options.mission_csv_file);
            input_description = options.mission_csv_file;
        } else {
            all_cases = load_mission_profile(options.environmental_csv_file, options.operating_csv_file);
            input_description = options.environmental_csv_file + " + " + options.operating_csv_file;
        }
        
        if (all_cases.empty()) {
            std::string error_msg = "No simulation cases found after filtering. Check input: " + input_description;
            throw std::runtime_error(error_msg);
        }
        
        std::cout << "\nSimulation Input Mode: " << options.input_mode << std::endl;
        std::cout << "Input Source: " << input_description << std::endl;
        if (options.input_mode == "static") {
            std::cout << "  Temperature: " << options.static_temperature << " C" << std::endl;
            std::cout << "  RH: " << options.static_rh << " %" << std::endl;
            std::cout << "  Voltage: " << options.static_voltage << " V" << std::endl;
            std::cout << "  Power: " << options.static_power << " W" << std::endl;
            std::cout << "  Irradiance: " << options.static_irradiance << " W/m^2" << std::endl;
        }
        std::cout << "Mission Profile Loaded: " << all_cases.size()
                  << " cases (after filtering GHI > 0 and ac_voltage > 0)" << std::endl;
        
        // ====================================================================
        // Section 4: Pre-load IV Curve Data for Mission Profile
        // ====================================================================
        // IVCurveData structure is defined above in namespace
        std::vector<IVCurveData> iv_curve_data;
        iv_curve_data.reserve(all_cases.size());
        
        // IVCurveSimulator converted to Python - commented out for now
        // TODO: Create C++ wrapper or integrate Python version
        // if (iv_simulator) {
        //     std::cout << "\nPre-loading IV curve data for mission profile..." << std::endl;
        //     int loaded_count = 0;
        //     
        //     for (size_t i = 0; i < all_cases.size(); ++i) {
        //         const auto& sim_case = all_cases[i];
        //         IVCurveData iv_data;
        //         iv_data.valid = false;
        //         
        //         // Get Voc and Isc for this mission profile point
        //         if (iv_simulator->get_voc_isc(
        //             sim_case.solar_irradiance, 
        //             sim_case.ambient_temperature, 
        //             iv_data.voc, 
        //             iv_data.isc)) {
        //             
        //             // Calculate operating voltage (e.g., MPP at ~0.8*Voc, or use DC-link voltage)
        //             // For now, use a simple MPP approximation: V_MPP ≈ 0.8 * Voc
        //             iv_data.pv_voltage = iv_data.voc * 0.8;
        //             
        //             // Get current at operating voltage
        //             iv_data.pv_current = iv_simulator->get_current(
        //                 sim_case.solar_irradiance,
        //                 sim_case.ambient_temperature,
        //                 iv_data.pv_voltage
        //             );
        //             
        //             if (iv_data.pv_current > 0) {
        //                 iv_data.valid = true;
        //                 loaded_count++;
        //             }
        //         }
        //         
        //         iv_curve_data.push_back(iv_data);
        //         
        //         // Progress indicator
        //         if ((i + 1) % 10000 == 0 || (i + 1) == all_cases.size()) {
        //             std::cout << "  Processed " << (i + 1) << "/" << all_cases.size() 
        //                       << " mission profile points..." << std::endl;
        //         }
        //     }
        //     
        //     std::cout << "  Successfully loaded IV curve data for " << loaded_count 
        //               << "/" << all_cases.size() << " cases" << std::endl;
        // } else {
            // No IV simulator - fill with invalid data (default constructor sets all to 0/false)
            iv_curve_data.resize(all_cases.size(), IVCurveData());
            std::cout << "\nWarning: No IV curve simulator available. Using simple PV voltage model." << std::endl;
        // }
        // ====================================================================
        
        // Create base simulation parameters from database
        // Database is required for all inverter and grid parameters
        SimulationParameters base_params;
        if (!sim_model.pv_inverter_part_number.empty() && !sim_model.grid_part_number.empty()) {
            // Load parameters from database
            base_params = create_parameters_from_database(
                component_db,
                sim_model.pv_inverter_part_number,
                sim_model.grid_part_number,
                options.modulation
            );
        } else {
            // Database profiles are required - cannot use fallback defaults
            std::string error_msg = "PV inverter and grid part numbers are required in simulation model.\n";
            error_msg += "  PV Inverter Part Number: " + 
                        (sim_model.pv_inverter_part_number.empty() ? "MISSING" : sim_model.pv_inverter_part_number) + "\n";
            error_msg += "  Grid Part Number: " + 
                        (sim_model.grid_part_number.empty() ? "MISSING" : sim_model.grid_part_number) + "\n";
            error_msg += "  Please ensure simulation_model.json contains valid part numbers.";
            throw std::runtime_error(error_msg);
        }
        
        // Extract topology_level and model_stage from loaded parameters (for compatibility)
        int topology_level = base_params.topology_level;
        int model_stage = base_params.model_stage;
        
        // Query all GPUs and aggregate their capacities
        AggregatedGpuCapacity agg_capacity = query_all_gpu_capacity();
        int available_gpus = agg_capacity.num_gpus;
        
        // Determine how many GPUs to use
        int num_gpus;
        if (options.num_gpus > 0) {
            // User specified number of GPUs
            if (options.num_gpus > available_gpus) {
                throw std::runtime_error("Requested " + std::to_string(options.num_gpus) + 
                                        " GPUs, but only " + std::to_string(available_gpus) + 
                                        " GPUs are available");
            }
            num_gpus = options.num_gpus;
        } else {
            // Use all available GPUs
            num_gpus = available_gpus;
        }
        
        // Limit the aggregated capacity to only the GPUs we'll use
        if (num_gpus < available_gpus) {
            // Recalculate aggregated capacity for only the GPUs we'll use
            agg_capacity.num_gpus = num_gpus;
            agg_capacity.total_threads = 0;
            agg_capacity.total_memory = 0;
            agg_capacity.total_multiprocessors = 0;
            agg_capacity.gpu_capacities.resize(num_gpus);
            
            for (int i = 0; i < num_gpus; ++i) {
                const auto& cap = agg_capacity.gpu_capacities[i];
                long long threads_per_gpu = static_cast<long long>(cap.multiprocessor_count) * 
                                            static_cast<long long>(cap.max_threads_per_multiprocessor);
                agg_capacity.total_threads += threads_per_gpu;
                agg_capacity.total_memory += cap.total_global_memory;
                agg_capacity.total_multiprocessors += cap.multiprocessor_count;
            }
        }
        
        std::cout << "GPU Configuration:" << std::endl;
        std::cout << "  Available GPUs: " << available_gpus << std::endl;
        std::cout << "  GPUs to use: " << num_gpus << std::endl;
        
        // Print individual GPU information (only for GPUs we'll use)
        long long total_threads_sum = 0;
        size_t total_memory_sum = 0;
        for (int i = 0; i < num_gpus; ++i) {
            const auto& cap = agg_capacity.gpu_capacities[i];
            long long threads_per_gpu = static_cast<long long>(cap.multiprocessor_count) * 
                                        static_cast<long long>(cap.max_threads_per_multiprocessor);
            total_threads_sum += threads_per_gpu;
            total_memory_sum += cap.total_global_memory;
            
            std::cout << "  GPU " << i << ": " << cap.device_name << std::endl;
            std::cout << "    Compute Capability: " << cap.compute_capability_major << "." 
                      << cap.compute_capability_minor << std::endl;
            std::cout << "    Multiprocessors: " << cap.multiprocessor_count << std::endl;
            std::cout << "    Threads: " << threads_per_gpu << " ("
                      << cap.multiprocessor_count << " MPs * " 
                      << cap.max_threads_per_multiprocessor << " threads/MP)" << std::endl;
            std::cout << "    Memory: " << std::fixed << std::setprecision(2)
                      << (cap.total_global_memory / (1024.0 * 1024.0 * 1024.0)) << " GB" << std::endl;
        }
        
        // Print aggregated totals
        std::cout << "\nAggregated Capacity (all GPUs):" << std::endl;
        std::cout << "  Total Multiprocessors: " << agg_capacity.total_multiprocessors << std::endl;
        std::cout << "  Total Threads: " << agg_capacity.total_threads << std::endl;
        std::cout << "  Total Memory: " << std::fixed << std::setprecision(2)
                  << (agg_capacity.total_memory / (1024.0 * 1024.0 * 1024.0)) << " GB" << std::endl;
        
        // The outer OpenMP region assigns one worker to each GPU. Reference
        // thermal post-processing uses an inner OpenMP team, so nested
        // parallelism must be enabled by the initial task before the GPU
        // workers are created. Otherwise a multi-GPU outer team serializes the
        // inner thermal loops, while the single-GPU (one-thread outer team)
        // path still runs them in parallel.
        omp_set_dynamic(0);
        omp_set_max_active_levels(2);
        omp_set_num_threads(num_gpus);
        
        // Calculate fundamental cycle size and batch size
        int fundamental_cycle_size = calculate_fundamental_cycle_size(base_params);
        
        // Calculate switching periods per fundamental cycle
        // Each switching period is allocated to one thread
        const double switching_periods_per_fundamental = base_params.switching_frequency / base_params.vg_freq;
        // Example: 16000 / 60 = 266.67 switching periods per fundamental cycle
        
        // Calculate batch size based on total threads across ALL GPUs
        // Total threads is already summed from all GPUs in agg_capacity
        const long long max_threads_total = agg_capacity.total_threads;
        
        // Each case needs switching_periods_per_fundamental threads (one thread per switching period)
        // So batch_size = max_threads_total / switching_periods_per_fundamental
        int batch_size_threads = static_cast<int>(max_threads_total / switching_periods_per_fundamental);
        
        // Also consider memory constraints - sum batch sizes from all GPUs
        int batch_size_memory = 0;
        for (const auto& cap : agg_capacity.gpu_capacities) {
            int batch_size_per_gpu = calculate_batch_size(cap, fundamental_cycle_size, 
                                                          base_params.num_states, sizeof(double));
            batch_size_memory += batch_size_per_gpu;
        }
        
        int batch_size_auto = std::min(batch_size_threads, batch_size_memory);
        int batch_size = batch_size_auto;
        if (options.batch_size_limit > 0) {
            batch_size = std::min(batch_size, options.batch_size_limit);
        }
        
        // Ensure batch_size is at least 1
        batch_size = std::max(1, batch_size);
        
        std::cout << "\nBatch Size Calculation:" << std::endl;
        std::cout << "  Total GPUs: " << num_gpus << std::endl;
        
        // Show per-GPU thread breakdown
        for (size_t i = 0; i < agg_capacity.gpu_capacities.size(); ++i) {
            const auto& cap = agg_capacity.gpu_capacities[i];
            long long threads_per_gpu = static_cast<long long>(cap.multiprocessor_count) * 
                                        static_cast<long long>(cap.max_threads_per_multiprocessor);
            std::cout << "  GPU " << i << " Threads: " << threads_per_gpu << " threads" << std::endl;
            std::cout << "    (" << cap.multiprocessor_count << " MPs * " 
                      << cap.max_threads_per_multiprocessor << " threads/MP)" << std::endl;
        }
        
        std::cout << "  Total Threads (all GPUs): " << max_threads_total << " threads" << std::endl;
        std::cout << "  Switching Periods per Fundamental: " << std::fixed << std::setprecision(2)
                  << switching_periods_per_fundamental << " periods" << std::endl;
        std::cout << "    (switching_freq/fundamental_freq = " << base_params.switching_frequency 
                  << "/" << base_params.vg_freq << " = " 
                  << switching_periods_per_fundamental << ")" << std::endl;
        std::cout << "  Fundamental Cycle Size: " << fundamental_cycle_size << " samples" << std::endl;
        std::cout << "    (samples per switching period = " 
                  << ((base_params.model_stage == 1) ? 7 : 8) << " intervals * " 
                  << base_params.a2s_points_per_interval << " samples = "
                  << ((base_params.model_stage == 1) ? 7 : 8) * base_params.a2s_points_per_interval 
                  << " samples)" << std::endl;
        std::cout << "  Batch Size (threads): " << batch_size_threads << " cases" << std::endl;
        std::cout << "    (total_threads / switching_periods = " << max_threads_total 
                  << " / " << switching_periods_per_fundamental << " = " 
                  << batch_size_threads << ")" << std::endl;
        std::cout << "  Batch Size (memory): " << batch_size_memory << " cases" << std::endl;
        std::cout << "    (sum of per-GPU batch sizes: ";
        bool first = true;
        for (size_t i = 0; i < agg_capacity.gpu_capacities.size(); ++i) {
            const auto& cap = agg_capacity.gpu_capacities[i];
            int batch_size_per_gpu = calculate_batch_size(cap, fundamental_cycle_size, 
                                                          base_params.num_states, sizeof(double));
            if (!first) std::cout << " + ";
            std::cout << "GPU" << i << ":" << batch_size_per_gpu;
            first = false;
        }
        std::cout << " = " << batch_size_memory << " cases)" << std::endl;
        std::cout << "  Batch Size Limit: "
                  << (options.batch_size_limit > 0 ? std::to_string(options.batch_size_limit) : std::string("auto"))
                  << std::endl;
        std::cout << "  Auto Batch Size: " << batch_size_auto << " cases per batch" << std::endl;
        std::cout << "  Final Batch Size: " << batch_size << " cases per batch" << std::endl;
        
        int total_cases = static_cast<int>(all_cases.size());
        // Persist the per-case thermal response for validation against field sensors.
        // Each OpenMP worker owns disjoint case indices, so these vectors are safely
        // populated in parallel and written after the round completes.
        std::vector<double> validation_capacitor_hotspot(total_cases, 0.0);
        std::vector<double> validation_capacitor_surface(total_cases, 0.0);
        std::vector<double> validation_internal_temperature(total_cases, 0.0);
        std::vector<double> validation_capacitor_loss(total_cases, 0.0);
        std::vector<double> validation_ac_power(total_cases, 0.0);
        std::vector<double> validation_igbt_junction(total_cases, 0.0);
        std::vector<int> validation_reference_model_used(total_cases, 0);
        if (options.num_rounds > total_cases) {
            throw std::runtime_error("Number of rounds cannot exceed the number of simulation cases.");
        }

        // Default to equal sequential case groups (used by static inputs and
        // timestamp formats that cannot be parsed).
        std::vector<int> round_case_boundaries(
            static_cast<std::size_t>(options.num_rounds + 1), 0);
        for (int boundary = 0; boundary <= options.num_rounds; ++boundary) {
            round_case_boundaries[static_cast<std::size_t>(boundary)] =
                static_cast<int>((static_cast<long long>(total_cases) * boundary) /
                                 options.num_rounds);
        }

        // A mission-profile round represents a fraction of the calendar year,
        // not merely a fraction of the daylight-filtered case count.  Thus six
        // rounds are Jan-Feb, Mar-Apr, ..., Nov-Dec even when the seasons have
        // different numbers of retained simulation cases.
        bool calendar_round_boundaries = options.input_mode == "mission";
        std::vector<double> calendar_coordinates;
        calendar_coordinates.reserve(all_cases.size());
        double previous_coordinate = -std::numeric_limits<double>::infinity();
        for (const SimulationCase& simulation_case : all_cases) {
            double coordinate = 0.0;
            if (!parse_calendar_month_coordinate(simulation_case.time, coordinate) ||
                coordinate < previous_coordinate) {
                calendar_round_boundaries = false;
                break;
            }
            calendar_coordinates.push_back(coordinate);
            previous_coordinate = coordinate;
        }
        if (calendar_round_boundaries && !calendar_coordinates.empty()) {
            const double calendar_year_start =
                std::floor(calendar_coordinates.front() / 12.0) * 12.0;
            for (int boundary = 1; boundary < options.num_rounds; ++boundary) {
                const double target = calendar_year_start +
                    12.0 * static_cast<double>(boundary) /
                        static_cast<double>(options.num_rounds);
                const auto it = std::lower_bound(
                    calendar_coordinates.begin(), calendar_coordinates.end(), target);
                round_case_boundaries[static_cast<std::size_t>(boundary)] =
                    static_cast<int>(it - calendar_coordinates.begin());
            }
            for (int boundary = 1; boundary <= options.num_rounds; ++boundary) {
                if (round_case_boundaries[static_cast<std::size_t>(boundary)] <=
                    round_case_boundaries[static_cast<std::size_t>(boundary - 1)]) {
                    calendar_round_boundaries = false;
                    break;
                }
            }
        }
        if (!calendar_round_boundaries) {
            for (int boundary = 0; boundary <= options.num_rounds; ++boundary) {
                round_case_boundaries[static_cast<std::size_t>(boundary)] =
                    static_cast<int>((static_cast<long long>(total_cases) * boundary) /
                                     options.num_rounds);
            }
        }

        std::vector<double> all_ambient_temps;
        std::vector<double> all_ambient_rhs;
        std::vector<double> all_environmental_loads;
        all_ambient_temps.reserve(all_cases.size());
        all_ambient_rhs.reserve(all_cases.size());
        all_environmental_loads.reserve(all_cases.size());

        bool has_any_ac_power = false;
        std::size_t missing_ac_power_cases = 0;
        double max_environmental_load = 0.0;
        for (const SimulationCase& sc : all_cases) {
            all_ambient_temps.push_back(sc.ambient_temperature);
            all_ambient_rhs.push_back(sc.rh);

            // Enclosure heating must be driven by power/loss, never silently by
            // irradiance. A missing power value is treated as zero and reported
            // so callers can provide a measured/calibrated thermal load.
            const double load_value = sc.has_ac_power ? std::max(0.0, sc.ac_power) : 0.0;
            has_any_ac_power = has_any_ac_power || sc.has_ac_power;
            missing_ac_power_cases += sc.has_ac_power ? 0 : 1;
            all_environmental_loads.push_back(load_value);
            if (std::isfinite(load_value) && load_value > max_environmental_load) {
                max_environmental_load = load_value;
            }
        }

        // The DDM environmental model uses load ratio. For power-driven static
        // cases, keep the DDM notebook's 10 kW reference floor so a single
        // low-power case is not normalized to full load.
        const double rated_environmental_load = std::max(max_environmental_load, 10000.0);

        if (missing_ac_power_cases > 0) {
            std::cerr << "Warning: " << missing_ac_power_cases
                      << " mission cases have no ac_power; enclosure heat load is set to zero "
                         "for those cases (GHI fallback is disabled)." << std::endl;
        }

        std::vector<double> all_internal_temps;
        std::vector<double> all_internal_rhs;
        calculate_internal_conditions_ddm(
            all_ambient_temps,
            all_ambient_rhs,
            all_environmental_loads,
            rated_environmental_load,
            all_internal_temps,
            all_internal_rhs
        );
        if (all_internal_temps.size() != all_cases.size() ||
            all_internal_rhs.size() != all_cases.size()) {
            throw std::runtime_error("DDM environmental model failed to generate internal conditions.");
        }
        std::size_t provided_internal_temperature_cases = 0;
        for (std::size_t i = 0; i < all_cases.size(); ++i) {
            if (all_cases[i].has_internal_temperature &&
                std::isfinite(all_cases[i].internal_temperature)) {
                all_internal_temps[i] = all_cases[i].internal_temperature;
                ++provided_internal_temperature_cases;
            }
        }

        if (!all_cases.empty()) {
            auto temp_minmax = std::minmax_element(all_internal_temps.begin(), all_internal_temps.end());
            auto rh_minmax = std::minmax_element(all_internal_rhs.begin(), all_internal_rhs.end());
            std::cout << "\nEnvironmental Simulation Model:" << std::endl;
            std::cout << "  Model: DDM physics-informed internal temperature/RH model" << std::endl;
            std::cout << "  Load input: "
                      << (has_any_ac_power ? "AC power (positive = exported to grid)"
                                           : "zero (AC power unavailable; GHI fallback disabled)")
                      << std::endl;
            std::cout << "  Internal temperature boundary: "
                      << (provided_internal_temperature_cases > 0
                              ? "provided mission-profile values override the environmental model"
                              : "environmental-model prediction")
                      << " (provided " << provided_internal_temperature_cases << "/"
                      << all_cases.size() << " cases)" << std::endl;
            std::cout << "  Rated load for normalization: " << rated_environmental_load << std::endl;
            std::cout << "  Internal temp range: [" << *temp_minmax.first << ", "
                      << *temp_minmax.second << "] C" << std::endl;
            std::cout << "  Internal RH range: [" << *rh_minmax.first << ", "
                      << *rh_minmax.second << "] %" << std::endl;
        }
        
        // Calculate threads per simulation case
        const int intervals_per_period = (model_stage == 1) ? 7 : 8;
        const int samples_per_interval = base_params.a2s_points_per_interval;
        const int samples_per_switching_period = intervals_per_period * samples_per_interval;
        const int periods = static_cast<int>(std::ceil(base_params.simulation_time / base_params.switching_period));
        const int num_samples_per_case = periods * samples_per_switching_period;
        const int threads_per_block = 256;
        const int blocks_per_case = (num_samples_per_case + threads_per_block - 1) / threads_per_block;
        const int threads_per_case = blocks_per_case * threads_per_block;
                
        std::cout << "\nSimulation Configuration:" << std::endl;
        std::cout << "  Topology: " << topology_level 
                  << "-level " << model_stage << "-stage" << std::endl;
        std::cout << "  Modulation: " << modulation_to_string(options.modulation) << std::endl;
        std::cout << "  Post-processing mode: " << options.post_processing_mode << std::endl;
        std::cout << "  Thermal step: " << options.thermal_step_s << " s" << std::endl;
        std::cout << "  Precision: " << precision_to_string(options.precision) << std::endl;
        std::cout << "  Pipeline: " << (options.pipeline_enabled ? "on" : "off") << std::endl;
        std::cout << "  Total Cases: " << total_cases << std::endl;
        std::cout << "  Batches per year (rounds): " << options.num_rounds << std::endl;
        std::cout << "  Nominal duration per batch: "
                  << (12.0 / static_cast<double>(options.num_rounds))
                  << " months" << std::endl;
        std::cout << "  Batch boundaries: "
                  << (calendar_round_boundaries
                          ? "calendar timestamps"
                          : "equal sequential case groups")
                  << std::endl;
        std::cout << std::endl;
        
        // Create output directories
        const std::string folder_name = std::to_string(topology_level) + "level_" +
                                        std::to_string(model_stage) + "stage";
        const std::filesystem::path output_dir = std::filesystem::path("outputs") /
                                                 folder_name /
                                                 modulation_to_string(options.modulation);
        std::filesystem::create_directories(output_dir);
        
        const std::filesystem::path log_dir = std::filesystem::path("logs");
        std::filesystem::create_directories(log_dir);
        
        // ====================================================================
        // Top-level while loop: Run mission profile repeatedly until one component
        // reaches degradation progress of 1.0
        // ====================================================================
        
        // Global accumulated stressors (across all mission profile iterations)
        double global_fan_stressor_electrical_external = 0.0;
        double global_fan_stressor_electrical_internal = 0.0;
        double global_fan_stressor_mechanical_external = 0.0;
        double global_fan_stressor_mechanical_internal = 0.0;
        double global_capacitor_stressor = 0.0;
        double global_igbt_stressor_deltaT = 0.0;  // IGBT stressor from deltaT model
        double global_igbt_stressor_arrhenius = 0.0;  // IGBT stressor from Arrhenius model
        double global_pcb_degradation = 0.0;
        
        // Global timing accumulators (across all iterations)
        double total_simulation_time = 0.0;
        std::vector<double> gpu_simulation_times(num_gpus, 0.0); // Track time per GPU
        ProfilingTotals profiling_totals;
        
        int mission_profile_iteration = 0;
        bool degradation_reached = false;
        std::string failed_component = "";
        
        // Storage for electrical simulation results from first iteration.
        // Electrical waveforms are reused across all subsequent mission-profile repeats.
        struct StoredElectricalResults {
            double I_cap_rms;
            std::vector<double> V_ce;
            std::vector<double> I_c;
            std::vector<double> I_cap;
            std::vector<double> time_points;
            double ac_power;
            std::array<IgbtGpuDeviceInput, 4> igbt_devices;
            double igbt_tavg = 0.0;
            double igbt_tsim = 0.0;
            bool has_capacitor_reference_inputs = false;
            bool has_igbt_reference_inputs = false;
            bool cache_on_disk = false;
            std::filesystem::path cache_file;
        };
        std::vector<StoredElectricalResults> stored_electrical_results(total_cases);
        const std::filesystem::path electrical_reference_cache_dir =
            output_dir / "electrical_reference_cache";
        std::filesystem::create_directories(electrical_reference_cache_dir);

        auto release_stored_reference_payload = [](StoredElectricalResults& stored) {
            clear_vector_storage(stored.V_ce);
            clear_vector_storage(stored.I_c);
            clear_vector_storage(stored.I_cap);
            clear_vector_storage(stored.time_points);
            for (auto& device : stored.igbt_devices) {
                clear_vector_storage(device.fundamental_i.t);
                clear_vector_storage(device.fundamental_i.y);
                clear_vector_storage(device.rising_i.t);
                clear_vector_storage(device.rising_i.y);
                clear_vector_storage(device.falling_v.t);
                clear_vector_storage(device.falling_v.y);
                clear_vector_storage(device.falling_i.t);
                clear_vector_storage(device.falling_i.y);
                clear_vector_storage(device.rising_v.t);
                clear_vector_storage(device.rising_v.y);
            }
        };

        auto write_stored_reference_cache = [&](StoredElectricalResults& stored,
                                                const std::filesystem::path& path) {
            // Full mission profiles have tens of thousands of cases. Persisting
            // waveform/device-level reference cache for every case is larger
            // than this deployment's RAM or system disk. The active exact cache
            // is the round-level degradation cache; per-case reference payloads
            // are treated as batch-local scratch data until a compact harmonic /
            // device-feature cache replaces this path.
            stored.cache_file.clear();
            stored.cache_on_disk = false;
            return false;
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                std::cerr << "Warning: cannot write electrical reference cache: "
                          << path << std::endl;
                return false;
            }
            const std::uint32_t magic = 0x54505643; // TPVC
            const std::uint32_t version = 1;
            out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
            out.write(reinterpret_cast<const char*>(&version), sizeof(version));
            out.write(reinterpret_cast<const char*>(&stored.I_cap_rms), sizeof(stored.I_cap_rms));
            out.write(reinterpret_cast<const char*>(&stored.ac_power), sizeof(stored.ac_power));
            out.write(reinterpret_cast<const char*>(&stored.igbt_tavg), sizeof(stored.igbt_tavg));
            out.write(reinterpret_cast<const char*>(&stored.igbt_tsim), sizeof(stored.igbt_tsim));
            out.write(reinterpret_cast<const char*>(&stored.has_capacitor_reference_inputs),
                      sizeof(stored.has_capacitor_reference_inputs));
            out.write(reinterpret_cast<const char*>(&stored.has_igbt_reference_inputs),
                      sizeof(stored.has_igbt_reference_inputs));
            write_double_vector(out, stored.time_points);
            write_double_vector(out, stored.I_cap);
            for (const auto& device : stored.igbt_devices) {
                write_igbt_device_input(out, device);
            }
            if (!out) {
                std::cerr << "Warning: incomplete electrical reference cache write: "
                          << path << std::endl;
                return false;
            }
            stored.cache_file = path;
            stored.cache_on_disk = true;
            return true;
        };

        auto read_stored_reference_cache = [&](StoredElectricalResults& stored) {
            if (!stored.cache_on_disk || stored.cache_file.empty()) {
                return true;
            }
            std::ifstream in(stored.cache_file, std::ios::binary);
            if (!in) {
                std::cerr << "Warning: cannot read electrical reference cache: "
                          << stored.cache_file << std::endl;
                return false;
            }
            std::uint32_t magic = 0;
            std::uint32_t version = 0;
            in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
            in.read(reinterpret_cast<char*>(&version), sizeof(version));
            if (!in || magic != 0x54505643 || version != 1) {
                std::cerr << "Warning: invalid electrical reference cache: "
                          << stored.cache_file << std::endl;
                return false;
            }
            in.read(reinterpret_cast<char*>(&stored.I_cap_rms), sizeof(stored.I_cap_rms));
            in.read(reinterpret_cast<char*>(&stored.ac_power), sizeof(stored.ac_power));
            in.read(reinterpret_cast<char*>(&stored.igbt_tavg), sizeof(stored.igbt_tavg));
            in.read(reinterpret_cast<char*>(&stored.igbt_tsim), sizeof(stored.igbt_tsim));
            in.read(reinterpret_cast<char*>(&stored.has_capacitor_reference_inputs),
                    sizeof(stored.has_capacitor_reference_inputs));
            in.read(reinterpret_cast<char*>(&stored.has_igbt_reference_inputs),
                    sizeof(stored.has_igbt_reference_inputs));
            if (!read_double_vector(in, stored.time_points) ||
                !read_double_vector(in, stored.I_cap)) {
                return false;
            }
            for (auto& device : stored.igbt_devices) {
                if (!read_igbt_device_input(in, device)) {
                    return false;
                }
            }
            return static_cast<bool>(in);
        };

        bool electrical_results_stored = false;
        const bool cache_electrical_results = options.max_iterations != 1;
        std::vector<RoundComputationCache> round_computation_cache(
            static_cast<std::size_t>(std::max(1, options.num_rounds)));
        const bool fast_post_processing = options.post_processing_mode == "fast";
        const bool reference_post_processing =
            options.post_processing_mode == "reference" || options.post_processing_mode == "hybrid";
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "Starting Degradation Tracking Simulation" << std::endl;
        std::cout << "Will run mission profile repeatedly until one component reaches degradation = 1.0" << std::endl;
        std::cout << "========================================\n" << std::endl;
        

        auto release_batch_memory = [](BatchOutputs& batch_outputs,
                                       std::vector<SimulationParameters>& params_batch) {
            for (UnifiedOutputs& outputs : batch_outputs.outputs) {
                std::vector<double>().swap(outputs.states);
                std::vector<double>().swap(outputs.time_points);
                std::vector<int>().swap(outputs.switching_states);
            }
            std::vector<UnifiedOutputs>().swap(batch_outputs.outputs);
            std::vector<SimulationParameters>().swap(params_batch);
#ifdef __linux__
            malloc_trim(0);
#endif
        };

        while (!degradation_reached &&
               (options.max_iterations <= 0 || mission_profile_iteration < options.max_iterations)) {
            mission_profile_iteration++;
            std::cout << "\n" << std::string(60, '=') << std::endl;
            std::cout << "Mission Profile Iteration #" << mission_profile_iteration << std::endl;
            std::cout << std::string(60, '=') << std::endl;
            
            // Check if electrical simulation can be skipped. Electrical waveforms depend on
            // mission/static operating conditions, not on accumulated degradation, so the
            // first iteration is cached and reused. Degradation-driven parameter changes are
            // handled at the loss/thermal round-cache layer below.
            bool skip_electrical_simulation = false;
            const bool static_electrical_profile = options.input_mode == "static";
            if (cache_electrical_results &&
                mission_profile_iteration > 1 && electrical_results_stored) {
                skip_electrical_simulation = true;
                std::cout << "Skipping electrical simulation (iteration " << mission_profile_iteration
                          << ", using stored electrical results from first iteration)" << std::endl;
            } else if (cache_electrical_results &&
                       mission_profile_iteration > 1 && !electrical_results_stored) {
                std::cout << "Warning: Cannot skip electrical simulation - results not stored yet. Running simulation." << std::endl;
            }
            
            // Reset case index for this mission profile iteration
            int case_index = 0;
            double iteration_simulation_time = 0.0;
            std::vector<double> iteration_gpu_simulation_times(num_gpus, 0.0); // Track time per GPU for this iteration
            int total_cases_processed = 0;
            std::vector<int> round_attempts(static_cast<std::size_t>(options.num_rounds), 0);
            
            // Mission profile iteration-level accumulated stressors (reset each iteration)
            double iteration_fan_stressor_electrical_external = 0.0;
            double iteration_fan_stressor_electrical_internal = 0.0;
            double iteration_fan_stressor_mechanical_external = 0.0;
            double iteration_fan_stressor_mechanical_internal = 0.0;
            double iteration_capacitor_stressor = 0.0;
            double iteration_igbt_stressor_deltaT = 0.0;  // IGBT stressor from deltaT model
            double iteration_igbt_stressor_arrhenius = 0.0;  // IGBT stressor from Arrhenius model
            double iteration_pcb_degradation = 0.0;

            auto current_degradation_values = [&]() {
                return DegradationValues{
                    global_fan_stressor_electrical_external + iteration_fan_stressor_electrical_external,
                    global_fan_stressor_electrical_internal + iteration_fan_stressor_electrical_internal,
                    global_fan_stressor_mechanical_external + iteration_fan_stressor_mechanical_external,
                    global_fan_stressor_mechanical_internal + iteration_fan_stressor_mechanical_internal,
                    global_capacitor_stressor + iteration_capacitor_stressor,
                    global_igbt_stressor_deltaT + iteration_igbt_stressor_deltaT,
                    global_igbt_stressor_arrhenius + iteration_igbt_stressor_arrhenius,
                    global_pcb_degradation + iteration_pcb_degradation};
            };

            const auto bucket_state_from_values = [](const DegradationValues& values) {
                const auto bucket = [](double value) {
                    constexpr double kBucketEpsilon = 1e-12;
                    const int bucket_index = static_cast<int>(std::floor(
                        std::max(0.0, value) / kCriticalDegradationStep + kBucketEpsilon));
                    // Thermal aging parameters are defined only through the
                    // component failure point (100% degradation).
                    return std::min(10, bucket_index);
                };
                return DegradationParameterState{
                    bucket(values[0]),
                    bucket(values[1]),
                    bucket(values[2]),
                    bucket(values[3]),
                    bucket(values[4]),
                    bucket(values[5]),
                    bucket(values[6]),
                    bucket(values[7])};
            };

            auto current_parameter_state = [&]() {
                return bucket_state_from_values(current_degradation_values());
            };

            const auto parameter_state_exceeds = [](const DegradationParameterState& candidate,
                                                    const DegradationParameterState& accepted) {
                for (std::size_t i = 0; i < candidate.size(); ++i) {
                    if (candidate[i] > accepted[i]) {
                        return true;
                    }
                }
                return false;
            };

            const auto describe_bucket_crossing = [](const DegradationParameterState& before,
                                                     const DegradationParameterState& after) {
                const std::array<const char*, 8> names = {
                    "Fan Electrical External",
                    "Fan Electrical Internal",
                    "Fan Mechanical External",
                    "Fan Mechanical Internal",
                    "Capacitor",
                    "IGBT DeltaT",
                    "IGBT Arrhenius",
                    "PCB"
                };
                std::ostringstream oss;
                bool first = true;
                for (std::size_t i = 0; i < names.size(); ++i) {
                    if (after[i] != before[i]) {
                        if (!first) {
                            oss << ", ";
                        }
                        oss << names[i] << " "
                            << static_cast<int>(std::lround(
                                   before[i] * kCriticalDegradationStep * 100.0))
                            << "%->"
                            << static_cast<int>(std::lround(
                                   after[i] * kCriticalDegradationStep * 100.0))
                            << "%";
                        first = false;
                    }
                }
                return first ? std::string("none") : oss.str();
            };
            
            const auto iteration_start = std::chrono::steady_clock::now();

            std::optional<StoredElectricalResults> static_electrical_result;
            double static_electrical_elapsed_s = 0.0;
            if (static_electrical_profile && !skip_electrical_simulation && !all_cases.empty()) {
                const auto static_cache_start = std::chrono::steady_clock::now();
                const SimulationCase& sc = all_cases.front();
                static const IVCurveData default_iv_data;
                const IVCurveData& iv_data = !iv_curve_data.empty() ? iv_curve_data.front() : default_iv_data;
                SimulationParameters static_params = base_params;
                update_params_for_case(static_params, sc, iv_data);
                std::vector<SimulationParameters> static_params_batch{static_params};
                BatchOutputs one_case_outputs = run_unified_gpu_batch(static_params_batch);
                if (!one_case_outputs.outputs.empty()) {
                    const UnifiedOutputs& outputs = one_case_outputs.outputs.front();
                    StressResults stress = calculate_stress(outputs, static_params);
                    double ac_power = sc.ac_power;
                    if (!sc.has_ac_power) {
                        ACPowerResults ac_power_results = calculate_ac_power(outputs, static_params);
                        ac_power = calculate_equivalent_ac_power(ac_power_results.p_AC_instantaneous);
                    }
                    StoredElectricalResults cached{};
                    cached.I_cap_rms = stress.I_cap_rms;
                    cached.V_ce = stress.V_ce;
                    cached.I_c = stress.I_c;
                    cached.I_cap = stress.I_cap;
                    cached.time_points = outputs.time_points;
                    cached.ac_power = ac_power;
                    cached.has_capacitor_reference_inputs =
                        !cached.time_points.empty() && !cached.I_cap.empty();
                    std::string igbt_cache_message;
                    cached.has_igbt_reference_inputs = build_igbt_reference_cached_input(
                        outputs,
                        static_params,
                        cached.igbt_devices,
                        cached.igbt_tavg,
                        cached.igbt_tsim,
                        igbt_cache_message);
                    if (!cached.has_igbt_reference_inputs && !igbt_cache_message.empty()) {
                        std::cerr << "Warning: static IGBT reference cache unavailable: "
                                  << igbt_cache_message << std::endl;
                    }
                    static_electrical_result = std::move(cached);
                    if (cache_electrical_results && !stored_electrical_results.empty()) {
                        stored_electrical_results.front() = *static_electrical_result;
                        const auto cache_path = electrical_reference_cache_dir /
                            "case_0.bin";
                        write_stored_reference_cache(stored_electrical_results.front(), cache_path);
                        release_stored_reference_payload(stored_electrical_results.front());
                    }
                    static_electrical_elapsed_s = one_case_outputs.elapsed_s;
                    iteration_simulation_time += static_electrical_elapsed_s;
                    std::cout << "Static electrical stress cached for this iteration." << std::endl;
                } else {
                    std::cout << "Warning: static electrical cache unavailable; falling back to batch electrical simulation." << std::endl;
                }
                profiling_totals.static_cache_electrical +=
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - static_cache_start).count();
            }
            
            for (int round = 0; round < options.num_rounds; ++round) {
            const auto round_start = std::chrono::steady_clock::now();

            const int round_start_case =
                round_case_boundaries[static_cast<std::size_t>(round)];
            const int round_end_case =
                round_case_boundaries[static_cast<std::size_t>(round + 1)];
            case_index = round_start_case;
            const int round_cases = round_end_case - round_start_case;
            const int round_attempt = ++round_attempts[static_cast<std::size_t>(round)];
            int cases_reported_in_round = 0;

            std::cout << "Starting Round " << (round + 1) << "/" << options.num_rounds
                      << ": " << round_cases << " cases" << std::endl;
            if (calendar_round_boundaries) {
                std::cout << "  Calendar range: " << all_cases[round_start_case].time
                          << " to " << all_cases[round_end_case - 1].time << std::endl;
            }
            std::cout << "TRACEPV_PROGRESS iteration=" << mission_profile_iteration
                      << " round=" << (round + 1) << "/" << options.num_rounds
                      << " pass=" << round_attempt
                      << " processed=0 total=" << round_cases << std::endl;
            
            // Process batches in this round
            int round_batches = (round_cases + batch_size - 1) / batch_size;
            int cases_processed_in_round = 0;
            
            // Round-level accumulated stressors (reset each round)
            double round_fan_stressor_electrical_external = 0.0;
            double round_fan_stressor_electrical_internal = 0.0;
            double round_fan_stressor_mechanical_external = 0.0;
            double round_fan_stressor_mechanical_internal = 0.0;
            double round_capacitor_stressor = 0.0;
            double round_igbt_stressor_deltaT = 0.0;  // IGBT stressor from deltaT model
            double round_igbt_stressor_arrhenius = 0.0;  // IGBT stressor from Arrhenius model
            double round_pcb_degradation = 0.0;
            bool round_terminated_early = false;  // Flag for early termination (shared across threads)
            
            std::vector<CaseStressorRecord> round_stressor_records;
            // A round is one sequential batch of the year.  Its electrical and
            // reliability results are committed exactly once.  The degradation
            // state present at the beginning of the batch selects the thermal
            // resistance used for that batch.
            const DegradationParameterState round_parameter_state = current_parameter_state();
            const double capacitor_thermal_resistance_scale =
                1.0 + static_cast<double>(round_parameter_state[4]) * kCriticalDegradationStep;
            const double igbt_thermal_resistance_scale =
                1.0 + static_cast<double>(std::max(round_parameter_state[5],
                                                   round_parameter_state[6])) *
                          kCriticalDegradationStep;
            RoundComputationCache& round_cache =
                round_computation_cache[static_cast<std::size_t>(round)];
            const bool reuse_round_cache =
                round_cache.valid && round_cache.parameter_state == round_parameter_state;

            std::string thermal_action = reuse_round_cache
                ? "reuse"
                : (round_cache.valid ? "updated" : "initial");
            std::cout << "TRACEPV_THERMAL action=" << thermal_action
                      << " iteration=" << mission_profile_iteration
                      << " round=" << (round + 1) << "/" << options.num_rounds
                      << " pass=" << round_attempt
                      << " critical_step_percent="
                      << static_cast<int>(std::lround(kCriticalDegradationStep * 100.0))
                      << std::endl;

            if (reuse_round_cache) {
                round_fan_stressor_electrical_external = round_cache.totals.fan_electrical_external;
                round_fan_stressor_electrical_internal = round_cache.totals.fan_electrical_internal;
                round_fan_stressor_mechanical_external = round_cache.totals.fan_mechanical_external;
                round_fan_stressor_mechanical_internal = round_cache.totals.fan_mechanical_internal;
                round_capacitor_stressor = round_cache.totals.capacitor;
                round_igbt_stressor_deltaT = round_cache.totals.igbt_deltaT;
                round_igbt_stressor_arrhenius = round_cache.totals.igbt_arrhenius;
                round_pcb_degradation = round_cache.totals.pcb;
                round_stressor_records = round_cache.records;
                cases_processed_in_round =
                    round_cache.cases_processed > 0 ? round_cache.cases_processed : round_cases;
                total_cases_processed += cases_processed_in_round;
                std::cout << "Reusing cached loss/thermal/reliability results for round "
                          << (round + 1)
                          << " (no critical degradation bucket crossed)."
                          << std::endl;
                cases_reported_in_round = round_cases;
                std::cout << "TRACEPV_PROGRESS iteration=" << mission_profile_iteration
                          << " round=" << (round + 1) << "/" << options.num_rounds
                          << " pass=" << round_attempt
                          << " processed=" << round_cases
                          << " total=" << round_cases << std::endl;
            } else if (round_cache.valid) {
                std::cout << "Thermal parameter bucket changed before round "
                          << (round + 1)
                          << "; processing this new batch with the updated thermal state."
                          << std::endl;
            }
            
            // Use OpenMP to distribute batches across GPUs
            if (!reuse_round_cache) {
            #pragma omp parallel
            {
                // Get CPU thread ID and assign to corresponding GPU
                int cpu_thread_id = omp_get_thread_num();
                
                // Only process if this thread has a GPU assigned
                if (cpu_thread_id < num_gpus) {
                    // Set this CPU thread to control its assigned GPU
                    cudaError_t err = cudaSetDevice(cpu_thread_id);
                    if (err == cudaSuccess) {
                        // Each GPU processes its assigned batches
                        // GPU 0: batches 0, num_gpus, 2*num_gpus, ...
                        // GPU 1: batches 1, num_gpus+1, 2*num_gpus+1, ...
                        // etc.
                        double thread_simulation_time = 0.0;
                        ProfilingTotals thread_profile;
                        int thread_cases_processed = 0;
                        
                        // Accumulate temperature data for rainflow counting (per thread)
                        std::vector<double> accumulated_junction_temps;  // For IGBT deltaT model
                        std::vector<double> accumulated_internal_temps;  // For PCB and IGBT Arrhenius model
                        std::vector<int> accumulated_junction_case_indices;  // Track which case each junction temp belongs to
                        std::vector<int> accumulated_internal_case_indices;  // Track which case each internal temp belongs to
                        // Track RH and voltage for Arrhenius model (corrosion/dendrites) - using internal temps
                        std::vector<double> accumulated_junction_rhs;  // RH for each junction temp (legacy, not used for Arrhenius)
                        std::vector<double> accumulated_junction_voltages;  // Voltage for each junction temp (legacy, not used for Arrhenius)
                        std::vector<double> accumulated_internal_rhs;  // RH for each internal temp (for Arrhenius model)
                        std::vector<double> accumulated_internal_voltages;  // Voltage for each internal temp (for Arrhenius model)
                        accumulated_junction_temps.reserve(round_cases);
                        accumulated_internal_temps.reserve(round_cases);
                        accumulated_junction_rhs.reserve(round_cases);
                        accumulated_junction_voltages.reserve(round_cases);
                        accumulated_internal_rhs.reserve(round_cases);
                        accumulated_internal_voltages.reserve(round_cases);
                        
                        // Accumulate stressors for degradation tracking (per thread)
                        // Fan: 4 stressors (electrical external, electrical internal, mechanical external, mechanical internal)
                        double fan_stressor_electrical_external = 0.0;
                        double fan_stressor_electrical_internal = 0.0;
                        double fan_stressor_mechanical_external = 0.0;
                        double fan_stressor_mechanical_internal = 0.0;
                        
                        // Capacitor: 1 stressor
                        double capacitor_stressor = 0.0;
                        
                        // IGBT: two stressors (deltaT model and Arrhenius model)
                        double igbt_stressor_deltaT = 0.0;  // Stressor from deltaT model
                        double igbt_stressor_arrhenius = 0.0;  // Stressor from Arrhenius model
                        
                        // PCB: accumulated degradation (1/Nf per cycle)
                        double pcb_degradation = 0.0;
                        
                        struct PreparedBatch {
                            int batch = 0;
                            int batch_start = 0;
                            int batch_end = 0;
                            int batch_cases = 0;
                            std::vector<SimulationParameters> params_batch;
                            BatchOutputs batch_outputs;
                            double electrical_wall_s = 0.0;
                        };

                        auto prepare_batch = [&](int batch) {
                            PreparedBatch prepared;
                            prepared.batch = batch;
                            prepared.batch_start = case_index + batch * batch_size;
                            prepared.batch_end = std::min(prepared.batch_start + batch_size, case_index + round_cases);
                            prepared.batch_cases = prepared.batch_end - prepared.batch_start;
                            if (prepared.batch_cases <= 0) {
                                return prepared;
                            }

                            prepared.params_batch.reserve(prepared.batch_cases);
                            for (int i = 0; i < prepared.batch_cases; ++i) {
                                int case_idx = prepared.batch_start + i;
                                if (case_idx >= total_cases) break;

                                const SimulationCase& sc = all_cases[case_idx];
                                static const IVCurveData default_iv_data;
                                const IVCurveData& iv_data = (case_idx < static_cast<int>(iv_curve_data.size())) ?
                                    iv_curve_data[case_idx] : default_iv_data;

                                SimulationParameters params = base_params;
                                update_params_for_case(params, sc, iv_data);
                                prepared.params_batch.push_back(params);
                            }
                            return prepared;
                        };

                        auto run_prepared_batch = [&](PreparedBatch prepared) {
                            if (prepared.batch_cases <= 0) {
                                return prepared;
                            }
                            if (static_electrical_result.has_value() || skip_electrical_simulation) {
                                prepared.batch_outputs.outputs.resize(prepared.params_batch.size());
                                prepared.batch_outputs.elapsed_s = 0.0;
                                return prepared;
                            }

                            cudaSetDevice(cpu_thread_id);
                            const auto electrical_start = std::chrono::steady_clock::now();
                            prepared.batch_outputs =
                                run_unified_gpu_batch(prepared.params_batch, options.precision);
                            prepared.electrical_wall_s =
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - electrical_start).count();
                            return prepared;
                        };

                        const bool use_pipeline =
                            options.pipeline_enabled &&
                            !static_electrical_result.has_value() &&
                            !skip_electrical_simulation &&
                            round_batches > 1;

                        std::future<PreparedBatch> next_batch_future;
                        bool next_batch_future_valid = false;
                        auto launch_batch = [&](int batch) {
                            PreparedBatch prepared = prepare_batch(batch);
                            if (!use_pipeline) {
                                return std::async(std::launch::deferred,
                                                  [prepared = std::move(prepared), &run_prepared_batch]() mutable {
                                                      return run_prepared_batch(std::move(prepared));
                                                  });
                            }
                            return std::async(std::launch::async,
                                              [prepared = std::move(prepared), &run_prepared_batch]() mutable {
                                                  return run_prepared_batch(std::move(prepared));
                                              });
                        };

                        if (use_pipeline) {
                            next_batch_future = launch_batch(cpu_thread_id);
                            next_batch_future_valid = true;
                        }

                        for (int batch = cpu_thread_id; batch < round_batches && !round_terminated_early; batch += num_gpus) {
                            PreparedBatch prepared;
                            if (use_pipeline) {
                                prepared = next_batch_future.get();
                                next_batch_future_valid = false;
                                const int next_batch = batch + num_gpus;
                                if (next_batch < round_batches && !round_terminated_early) {
                                    next_batch_future = launch_batch(next_batch);
                                    next_batch_future_valid = true;
                                }
                            } else {
                                prepared = run_prepared_batch(prepare_batch(batch));
                            }

                            int batch_start = prepared.batch_start;
                            int batch_end = prepared.batch_end;
                            int batch_cases = prepared.batch_cases;
                            std::vector<SimulationParameters> params_batch = std::move(prepared.params_batch);
                            BatchOutputs batch_outputs = std::move(prepared.batch_outputs);
                            std::vector<int> loaded_cache_indices;

                            if (batch_cases <= 0) continue;

                            thread_profile.electrical_gpu += prepared.electrical_wall_s;
                            
                            // Collect thermal data for batch reliability assessment
                            std::vector<double> batch_ambient_temps;
                            std::vector<double> batch_ambient_rhs;
                            std::vector<double> batch_ac_voltages;
                            std::vector<double> batch_capacitor_hotspot_temps;
                            std::vector<double> batch_capacitor_voltages;
                            std::vector<double> batch_junction_temps;
                            
                            batch_ambient_temps.reserve(batch_outputs.outputs.size());
                            batch_ambient_rhs.reserve(batch_outputs.outputs.size());
                            batch_ac_voltages.reserve(batch_outputs.outputs.size());
                            batch_capacitor_hotspot_temps.reserve(batch_outputs.outputs.size());
                            batch_capacitor_voltages.reserve(batch_outputs.outputs.size());
                            batch_junction_temps.reserve(batch_outputs.outputs.size());
                            
                            // Load capacitor coefficients once for the batch (if needed)
                            CapacitorCoefficients cap_coeffs;
                            CapacitorType cap_type;
                            std::string cap_voltage_type;
                            bool cap_coeffs_loaded = false;
                            if (!sim_model.capacitor_part_number.empty()) {
                                cap_coeffs_loaded = component_db.load_capacitor(sim_model.capacitor_part_number, cap_coeffs, cap_type, cap_voltage_type);
                                if (cap_coeffs_loaded) {
                                    cap_coeffs.type = cap_type;
                                }
                            }
                            
                            // Collect ambient data for reliability kernels.
                            // Internal conditions are precomputed once for the full mission
                            // profile with the DDM environmental model to preserve time-series
                            // EWMA/rolling state across GPU batches.
                            std::vector<double> batch_internal_temps;
                            std::vector<double> batch_internal_rhs;
                            batch_internal_temps.reserve(batch_outputs.outputs.size());
                            batch_internal_rhs.reserve(batch_outputs.outputs.size());
                            for (size_t case_idx = 0; case_idx < batch_outputs.outputs.size(); ++case_idx) {
                                int actual_case_idx = batch_start + static_cast<int>(case_idx);
                                const SimulationCase& sc = all_cases[actual_case_idx];
                                batch_ambient_temps.push_back(sc.ambient_temperature);
                                batch_ambient_rhs.push_back(sc.rh);
                                batch_ac_voltages.push_back(sc.ac_voltage);
                                batch_internal_temps.push_back(all_internal_temps[actual_case_idx]);
                                batch_internal_rhs.push_back(all_internal_rhs[actual_case_idx]);
                            }
                            
                            const auto stress_loss_thermal_start = std::chrono::steady_clock::now();
                            std::vector<StressResults> batch_stresses(batch_outputs.outputs.size());
                            std::vector<double> batch_ac_powers(batch_outputs.outputs.size(), 0.0);
                            for (size_t case_idx = 0; case_idx < batch_outputs.outputs.size(); ++case_idx) {
                                const SimulationParameters& params = params_batch[case_idx];
                                int actual_case_idx = batch_start + static_cast<int>(case_idx);
                                const SimulationCase& sc = all_cases[actual_case_idx];
                                StressResults& stress = batch_stresses[case_idx];
                                double& ac_power = batch_ac_powers[case_idx];

                                const auto stress_calculation_start = std::chrono::steady_clock::now();
                                if (static_electrical_result.has_value()) {
                                    const StoredElectricalResults& stored = *static_electrical_result;
                                    stress.I_cap_rms = stored.I_cap_rms;
                                    stress.V_ce = stored.V_ce;
                                    stress.I_c = stored.I_c;
                                    stress.I_cap = stored.I_cap;
                                    ac_power = stored.ac_power;
                                    if (cache_electrical_results && !static_electrical_profile &&
                                        actual_case_idx < static_cast<int>(stored_electrical_results.size())) {
                                        stored_electrical_results[actual_case_idx] = stored;
                                    }
                                } else if (skip_electrical_simulation) {
                                    const int stored_case_idx = static_electrical_profile ? 0 : actual_case_idx;
                                    if (stored_case_idx < static_cast<int>(stored_electrical_results.size())) {
                                        StoredElectricalResults& stored = stored_electrical_results[stored_case_idx];
                                        if (stored.cache_on_disk &&
                                            stored.time_points.empty() &&
                                            stored.I_cap.empty()) {
                                            if (read_stored_reference_cache(stored)) {
                                                loaded_cache_indices.push_back(stored_case_idx);
                                            }
                                        }
                                        stress.I_cap_rms = stored.I_cap_rms;
                                        stress.V_ce = stored.V_ce;
                                        stress.I_c = stored.I_c;
                                        stress.I_cap = stored.I_cap;
                                        ac_power = stored.ac_power;
                                    } else {
                                        std::cerr << "Warning: Case index " << actual_case_idx
                                                  << " out of range for stored results. Using zero values." << std::endl;
                                        stress.I_cap_rms = 0.0;
                                        stress.V_ce.clear();
                                        stress.I_c.clear();
                                        stress.I_cap.clear();
                                        ac_power = 0.0;
                                    }
                                } else {
                                    const UnifiedOutputs& outputs = batch_outputs.outputs[case_idx];
                                    stress = calculate_stress(outputs, params);
                                    if (sc.has_ac_power) {
                                        ac_power = sc.ac_power;
                                    } else {
                                        ACPowerResults ac_power_results = calculate_ac_power(outputs, params);
                                        ac_power = calculate_equivalent_ac_power(ac_power_results.p_AC_instantaneous);
                                    }

                                    if (cache_electrical_results &&
                                        actual_case_idx < static_cast<int>(stored_electrical_results.size())) {
                                        if (mission_profile_iteration == 1) {
                                            stored_electrical_results[actual_case_idx].I_cap_rms = stress.I_cap_rms;
	                                            stored_electrical_results[actual_case_idx].V_ce = stress.V_ce;
	                                            stored_electrical_results[actual_case_idx].I_c = stress.I_c;
	                                            stored_electrical_results[actual_case_idx].I_cap = stress.I_cap;
	                                            stored_electrical_results[actual_case_idx].time_points = outputs.time_points;
	                                            stored_electrical_results[actual_case_idx].ac_power = ac_power;
	                                            stored_electrical_results[actual_case_idx].has_capacitor_reference_inputs =
	                                                !stored_electrical_results[actual_case_idx].time_points.empty() &&
	                                                !stored_electrical_results[actual_case_idx].I_cap.empty();
	                                            std::string igbt_cache_message;
	                                            stored_electrical_results[actual_case_idx].has_igbt_reference_inputs =
	                                                build_igbt_reference_cached_input(
	                                                    outputs,
	                                                    params,
	                                                    stored_electrical_results[actual_case_idx].igbt_devices,
	                                                    stored_electrical_results[actual_case_idx].igbt_tavg,
	                                                    stored_electrical_results[actual_case_idx].igbt_tsim,
	                                                    igbt_cache_message);
	                                            if (!stored_electrical_results[actual_case_idx].has_igbt_reference_inputs &&
	                                                !igbt_cache_message.empty()) {
	                                                std::cerr << "Warning: IGBT reference cache unavailable for case "
	                                                          << actual_case_idx << ": " << igbt_cache_message << std::endl;
	                                            }
	                                            const auto cache_path = electrical_reference_cache_dir /
	                                                ("case_" + std::to_string(actual_case_idx) + ".bin");
	                                            write_stored_reference_cache(
	                                                stored_electrical_results[actual_case_idx],
	                                                cache_path);
	                                            release_stored_reference_payload(
	                                                stored_electrical_results[actual_case_idx]);
	                                        } else if (mission_profile_iteration > 10) {
	                                            stored_electrical_results[actual_case_idx].I_cap_rms = stress.I_cap_rms;
	                                            stored_electrical_results[actual_case_idx].V_ce = stress.V_ce;
	                                            stored_electrical_results[actual_case_idx].I_c = stress.I_c;
	                                            stored_electrical_results[actual_case_idx].I_cap = stress.I_cap;
	                                            stored_electrical_results[actual_case_idx].time_points = outputs.time_points;
	                                            stored_electrical_results[actual_case_idx].ac_power = ac_power;
	                                            stored_electrical_results[actual_case_idx].has_capacitor_reference_inputs =
	                                                !stored_electrical_results[actual_case_idx].time_points.empty() &&
	                                                !stored_electrical_results[actual_case_idx].I_cap.empty();
	                                            std::string igbt_cache_message;
	                                            stored_electrical_results[actual_case_idx].has_igbt_reference_inputs =
	                                                build_igbt_reference_cached_input(
	                                                    outputs,
	                                                    params,
	                                                    stored_electrical_results[actual_case_idx].igbt_devices,
	                                                    stored_electrical_results[actual_case_idx].igbt_tavg,
	                                                    stored_electrical_results[actual_case_idx].igbt_tsim,
	                                                    igbt_cache_message);
	                                            if (!stored_electrical_results[actual_case_idx].has_igbt_reference_inputs &&
	                                                !igbt_cache_message.empty()) {
	                                                std::cerr << "Warning: IGBT reference cache unavailable for case "
	                                                          << actual_case_idx << ": " << igbt_cache_message << std::endl;
	                                            }
	                                            const auto cache_path = electrical_reference_cache_dir /
	                                                ("case_" + std::to_string(actual_case_idx) + ".bin");
	                                            write_stored_reference_cache(
	                                                stored_electrical_results[actual_case_idx],
	                                                cache_path);
	                                            release_stored_reference_payload(
	                                                stored_electrical_results[actual_case_idx]);
	                                        }
                                    }
                                }
                                thread_profile.stress_calculation +=
                                    std::chrono::duration<double>(std::chrono::steady_clock::now() - stress_calculation_start).count();
                            }

                            std::vector<CapacitorReferenceThermalResult> batch_capacitor_reference_thermal(batch_outputs.outputs.size());
                            if (reference_post_processing) {
                                const auto capacitor_reference_batch_start = std::chrono::steady_clock::now();
                                std::vector<CapacitorReferenceThermalInput> capacitor_reference_inputs(batch_outputs.outputs.size());
                                for (size_t case_idx = 0; case_idx < batch_outputs.outputs.size(); ++case_idx) {
                                    const int actual_case_idx = batch_start + static_cast<int>(case_idx);
                                    const StoredElectricalResults* stored_reference = nullptr;
                                    if (static_electrical_result.has_value()) {
                                        stored_reference = &*static_electrical_result;
                                    } else if (skip_electrical_simulation) {
                                        const int stored_case_idx = static_electrical_profile ? 0 : actual_case_idx;
                                        if (stored_case_idx >= 0 &&
                                            stored_case_idx < static_cast<int>(stored_electrical_results.size())) {
                                            stored_reference = &stored_electrical_results[stored_case_idx];
                                        }
                                    }
                                    double rth_amb_value = cap_coeffs_loaded ? cap_coeffs.rth_amb : 0.5;
                                    double rth_surf_value = cap_coeffs_loaded ? cap_coeffs.rth_surf : 0.3;
                                    rth_amb_value *= capacitor_thermal_resistance_scale;
                                    rth_surf_value *= capacitor_thermal_resistance_scale;
                                    double rth_core_surface = std::max(0.0, rth_amb_value - rth_surf_value);
                                    if (stored_reference && stored_reference->has_capacitor_reference_inputs) {
                                        capacitor_reference_inputs[case_idx].time_points = &stored_reference->time_points;
                                        capacitor_reference_inputs[case_idx].capacitor_current = &stored_reference->I_cap;
                                    } else if (!skip_electrical_simulation &&
                                               case_idx < batch_outputs.outputs.size()) {
                                        capacitor_reference_inputs[case_idx].time_points = &batch_outputs.outputs[case_idx].time_points;
                                        capacitor_reference_inputs[case_idx].capacitor_current = &batch_stresses[case_idx].I_cap;
                                    }
                                    capacitor_reference_inputs[case_idx].ambient_temperature = batch_internal_temps[case_idx];
                                    capacitor_reference_inputs[case_idx].rth_surface_ambient = rth_surf_value;
                                    capacitor_reference_inputs[case_idx].rth_core_surface = rth_core_surface;
                                }
                                batch_capacitor_reference_thermal =
                                    calculate_capacitor_reference_thermal_batch(capacitor_reference_inputs);
                                thread_profile.capacitor_thermal +=
                                    std::chrono::duration<double>(std::chrono::steady_clock::now() - capacitor_reference_batch_start).count();
                                for (const auto& capacitor_reference_thermal : batch_capacitor_reference_thermal) {
                                    thread_profile.capacitor_ref_harmonic +=
                                        capacitor_reference_thermal.harmonic_extraction_s;
                                    thread_profile.capacitor_ref_esr_grid +=
                                        capacitor_reference_thermal.esr_grid_s;
                                    thread_profile.capacitor_ref_loss_grid +=
                                        capacitor_reference_thermal.loss_grid_s;
                                    thread_profile.capacitor_ref_polyfit +=
                                        capacitor_reference_thermal.polyfit_s;
                                    thread_profile.capacitor_ref_iteration +=
                                        capacitor_reference_thermal.thermal_iteration_s;
                                }
                            }

                            std::vector<IgbtReferenceThermalResult> batch_igbt_reference_thermal(batch_outputs.outputs.size());
                            if (reference_post_processing) {
                                const auto igbt_reference_batch_start = std::chrono::steady_clock::now();
                                std::vector<IgbtReferenceThermalInput> igbt_reference_inputs(batch_outputs.outputs.size());
                                std::vector<IgbtReferenceCachedThermalInput> cached_igbt_reference_inputs(batch_outputs.outputs.size());
                                bool use_cached_igbt_reference_inputs = false;
                                for (size_t case_idx = 0; case_idx < batch_outputs.outputs.size(); ++case_idx) {
                                    const int actual_case_idx = batch_start + static_cast<int>(case_idx);
                                    const StoredElectricalResults* stored_reference = nullptr;
                                    if (static_electrical_result.has_value()) {
                                        stored_reference = &*static_electrical_result;
                                    } else if (skip_electrical_simulation) {
                                        const int stored_case_idx = static_electrical_profile ? 0 : actual_case_idx;
                                        if (stored_case_idx >= 0 &&
                                            stored_case_idx < static_cast<int>(stored_electrical_results.size())) {
                                            stored_reference = &stored_electrical_results[stored_case_idx];
                                        }
                                    }
                                    if (stored_reference && stored_reference->has_igbt_reference_inputs) {
                                        cached_igbt_reference_inputs[case_idx].devices = &stored_reference->igbt_devices;
                                        cached_igbt_reference_inputs[case_idx].tavg = stored_reference->igbt_tavg;
                                        cached_igbt_reference_inputs[case_idx].tsim = stored_reference->igbt_tsim;
                                        cached_igbt_reference_inputs[case_idx].ambient_temperature =
                                            all_cases[batch_start + static_cast<int>(case_idx)].ambient_temperature;
                                        use_cached_igbt_reference_inputs = true;
                                    } else if (!skip_electrical_simulation &&
                                               case_idx < batch_outputs.outputs.size()) {
                                        igbt_reference_inputs[case_idx].outputs = &batch_outputs.outputs[case_idx];
                                        igbt_reference_inputs[case_idx].params = &params_batch[case_idx];
                                    }
                                    igbt_reference_inputs[case_idx].ambient_temperature =
                                        all_cases[batch_start + static_cast<int>(case_idx)].ambient_temperature;
                                }
                                if (use_cached_igbt_reference_inputs) {
                                    batch_igbt_reference_thermal = calculate_igbt_reference_thermal_batch_cached(
                                        cached_igbt_reference_inputs,
                                        "IGBT/MATLAB_code/parameters",
                                        20.0,
                                        options.thermal_step_s);
                                } else {
                                    batch_igbt_reference_thermal = calculate_igbt_reference_thermal_batch(
                                        igbt_reference_inputs,
                                        "IGBT/MATLAB_code/parameters",
                                        20.0,
                                        options.thermal_step_s);
                                }
                                thread_profile.igbt_thermal +=
                                    std::chrono::duration<double>(std::chrono::steady_clock::now() - igbt_reference_batch_start).count();
                                for (const auto& igbt_reference_thermal : batch_igbt_reference_thermal) {
                                    thread_profile.igbt_parameter_lookup += igbt_reference_thermal.parameter_lookup_s;
                                    thread_profile.igbt_input_build += igbt_reference_thermal.input_build_s;
                                    thread_profile.igbt_loss_igbt1 += igbt_reference_thermal.loss_igbt1_s;
                                    thread_profile.igbt_loss_igbt2 += igbt_reference_thermal.loss_igbt2_s;
                                    thread_profile.igbt_loss_diode1 += igbt_reference_thermal.loss_diode1_s;
                                    thread_profile.igbt_loss_diode2 += igbt_reference_thermal.loss_diode2_s;
                                    thread_profile.igbt_thermal_rc += igbt_reference_thermal.thermal_rc_s;
                                }
                            }

                            // Process loss model and thermal model for each case
                            for (size_t case_idx = 0; case_idx < batch_outputs.outputs.size(); ++case_idx) {
                                const SimulationParameters& params = params_batch[case_idx];
                                
                                // Get the corresponding simulation case for ambient conditions
                                int actual_case_idx = batch_start + static_cast<int>(case_idx);
                                const SimulationCase& sc = all_cases[actual_case_idx];
                                const StressResults& stress = batch_stresses[case_idx];
                                double ac_power = batch_ac_powers[case_idx];
                                
                                // Calculate losses from stress waveforms
                                // 2.1 Capacitor loss model: input I_cap_rms and ESR, output capacitor loss
                                // For 3-level topology: two capacitors in series, so effective ESR is doubled
                                double esr_value = cap_coeffs_loaded ? cap_coeffs.esr : 0.01; // Use default if not loaded
                                if (params.topology_level == 3) {
                                    esr_value = esr_value * 2.0;  // Two capacitors in series: ESR_total = ESR1 + ESR2
                                }
                                const auto capacitor_loss_start = std::chrono::steady_clock::now();
                                CapacitorLossResult capacitor_loss = calculate_capacitor_loss(stress.I_cap_rms, esr_value);
                                thread_profile.capacitor_loss +=
                                    std::chrono::duration<double>(std::chrono::steady_clock::now() - capacitor_loss_start).count();
                                
                                // Debug: Print capacitor loss calculation details
                                static bool loss_debug_printed = false;
                                if (!loss_debug_printed && case_idx == 0) {
                                    std::cerr << "DEBUG: Capacitor loss calculation:" << std::endl;
                                    std::cerr << "  I_cap_rms: " << stress.I_cap_rms << " A" << std::endl;
                                    std::cerr << "  ESR: " << esr_value << " Ohm" << std::endl;
                                    std::cerr << "  Capacitor loss: " << capacitor_loss.capacitor_loss << " W" << std::endl;
                                    loss_debug_printed = true;
                                }
                                
                                // 2.2 Power module loss is obtained from the reference IGBT loss/thermal model below.
                                // The legacy V_ce/I_c-only API is intentionally not used for accurate simulation.
                                PowerModuleLossResult power_module_loss;
                                
                                // Calculate thermal response from losses
                                // 2.3 Capacitor thermal model: input capacitor loss, internal temp, rth_amb, rth_surf
                                //     output capacitor hotspot temp and capacitor surface temp
                                double rth_amb_value = cap_coeffs_loaded ? cap_coeffs.rth_amb : 0.5; // Use default if not loaded
                                double rth_surf_value = cap_coeffs_loaded ? cap_coeffs.rth_surf : 0.3; // Use default if not loaded
                                rth_amb_value *= capacitor_thermal_resistance_scale;
                                rth_surf_value *= capacitor_thermal_resistance_scale;
                                double internal_temp = batch_internal_temps[case_idx];
                                
                                // Debug: Print thermal model inputs
                                static bool thermal_debug_printed = false;
                                if (!thermal_debug_printed && case_idx == 0) {
                                    std::cerr << "DEBUG: Capacitor thermal model inputs:" << std::endl;
                                    std::cerr << "  Capacitor loss: " << capacitor_loss.capacitor_loss << " W" << std::endl;
	                                    std::cerr << "  Internal temp: " << internal_temp << " C" << std::endl;
	                                    std::cerr << "  rth_amb: " << rth_amb_value << " K/W" << std::endl;
	                                    std::cerr << "  rth_surf: " << rth_surf_value << " K/W" << std::endl;
	                                }
                                
                                const auto capacitor_thermal_start = std::chrono::steady_clock::now();
                                CapacitorReferenceThermalResult capacitor_reference_thermal{};
                                if (reference_post_processing &&
                                    case_idx < batch_capacitor_reference_thermal.size()) {
                                    capacitor_reference_thermal = batch_capacitor_reference_thermal[case_idx];
                                    if (capacitor_reference_thermal.valid &&
                                        (capacitor_reference_thermal.hotspot_temperature > 200.0 ||
                                         capacitor_reference_thermal.hotspot_temperature - internal_temp > 150.0 ||
                                         capacitor_reference_thermal.loss > std::max(10.0, 20.0 * capacitor_loss.capacitor_loss))) {
                                        capacitor_reference_thermal.valid = false;
                                        capacitor_reference_thermal.message =
                                            "reference harmonic ESR result rejected by sanity check; using database ESR thermal model";
                                    }
                                }

                                CapacitorThermalResult capacitor_thermal{};
                                if (capacitor_reference_thermal.valid) {
                                    capacitor_loss.capacitor_loss = capacitor_reference_thermal.loss;
                                    capacitor_thermal.capacitor_hotspot_temperature =
                                        capacitor_reference_thermal.hotspot_temperature;
                                    capacitor_thermal.capacitor_surface_temperature =
                                        capacitor_reference_thermal.surface_temperature;
                                } else {
                                    const auto capacitor_fallback_start = std::chrono::steady_clock::now();
                                    capacitor_thermal = calculate_capacitor_thermal(
                                        capacitor_loss.capacitor_loss,
                                        internal_temp,
                                        rth_amb_value,
                                        rth_surf_value
                                    );
                                    thread_profile.capacitor_fallback_static +=
                                        std::chrono::duration<double>(std::chrono::steady_clock::now() - capacitor_fallback_start).count();
                                }
                                thread_profile.capacitor_thermal +=
                                    std::chrono::duration<double>(std::chrono::steady_clock::now() - capacitor_thermal_start).count();
                                
                                // Debug: Print thermal model output
                                if (!thermal_debug_printed && case_idx == 0) {
                                    std::cerr << "DEBUG: Capacitor thermal model output:" << std::endl;
                                    std::cerr << "  Model: " << (capacitor_reference_thermal.valid ? capacitor_reference_thermal.message : "fallback RMS ESR/static thermal model") << std::endl;
                                    if (!capacitor_reference_thermal.valid && !capacitor_reference_thermal.message.empty()) {
                                        std::cerr << "  Reference model unavailable: " << capacitor_reference_thermal.message << std::endl;
                                    }
	                                    std::cerr << "  Hotspot temp: " << capacitor_thermal.capacitor_hotspot_temperature << " C" << std::endl;
	                                    std::cerr << "  Surface temp: " << capacitor_thermal.capacitor_surface_temperature << " C" << std::endl;
	                                    thermal_debug_printed = true;
	                                }
                                
                                // 2.4 Power module thermal model.
                                // Prefer the reference IGBT loss + RC thermal model when full A2S waveforms are available.
                                const auto igbt_thermal_start = std::chrono::steady_clock::now();
                                PowerModuleThermalResult power_module_thermal{};
                                IgbtReferenceThermalResult igbt_reference_thermal{};
                                if (reference_post_processing &&
                                    case_idx < batch_igbt_reference_thermal.size()) {
                                    igbt_reference_thermal = batch_igbt_reference_thermal[case_idx];
                                }
                                if (igbt_reference_thermal.valid) {
                                    power_module_loss.power_module_loss =
                                        igbt_reference_thermal.average_total_loss;
                                    power_module_loss.valid = true;
                                    power_module_loss.message = igbt_reference_thermal.message;
                                    power_module_thermal.junction_temperature =
                                        sc.ambient_temperature +
                                        (igbt_reference_thermal.junction_temperature -
                                         sc.ambient_temperature) *
                                            igbt_thermal_resistance_scale;
                                } else {
                                    power_module_thermal = calculate_power_module_thermal(
                                        ac_power,
                                        sc.ambient_temperature
                                    );
                                    power_module_thermal.junction_temperature =
                                        sc.ambient_temperature +
                                        (power_module_thermal.junction_temperature -
                                         sc.ambient_temperature) *
                                            igbt_thermal_resistance_scale;
                                }
                                thread_profile.igbt_thermal +=
                                    std::chrono::duration<double>(std::chrono::steady_clock::now() - igbt_thermal_start).count();
                                
                                // Debug: Print IGBT thermal model output
                                static bool igbt_thermal_debug_printed = false;
                                if (!igbt_thermal_debug_printed && case_idx == 0) {
                                    std::cerr << "DEBUG: IGBT thermal model output:" << std::endl;
                                    std::cerr << "  Model: " << (igbt_reference_thermal.valid ? igbt_reference_thermal.message : "fallback AC-power linear model") << std::endl;
                                    if (!igbt_reference_thermal.valid && !igbt_reference_thermal.message.empty()) {
                                        std::cerr << "  Reference model unavailable: " << igbt_reference_thermal.message << std::endl;
                                    }
                                    std::cerr << "  AC power (thermal input): " << ac_power << " W" << std::endl;
                                    std::cerr << "  Ambient temp: " << sc.ambient_temperature << " C" << std::endl;
                                    std::cerr << "  Junction temp: " << power_module_thermal.junction_temperature << " C" << std::endl;
                                    if (igbt_reference_thermal.valid) {
                                        std::cerr << "  Tj devices: IGBT1=" << igbt_reference_thermal.igbt1_temperature
                                                  << " C, IGBT2=" << igbt_reference_thermal.igbt2_temperature
                                                  << " C, Diode1=" << igbt_reference_thermal.diode1_temperature
                                                  << " C, Diode2=" << igbt_reference_thermal.diode2_temperature << " C" << std::endl;
                                        std::cerr << "  Avg inverter loss: " << igbt_reference_thermal.average_total_loss << " W" << std::endl;
                                    }
                                    igbt_thermal_debug_printed = true;
                                }
                                
                                // Collect data for batch reliability assessment
                                validation_capacitor_hotspot[actual_case_idx] =
                                    capacitor_thermal.capacitor_hotspot_temperature;
                                validation_capacitor_surface[actual_case_idx] =
                                    capacitor_thermal.capacitor_surface_temperature;
                                validation_internal_temperature[actual_case_idx] = internal_temp;
                                validation_capacitor_loss[actual_case_idx] =
                                    capacitor_loss.capacitor_loss;
                                validation_ac_power[actual_case_idx] = ac_power;
                                validation_igbt_junction[actual_case_idx] =
                                    power_module_thermal.junction_temperature;
                                validation_reference_model_used[actual_case_idx] =
                                    capacitor_reference_thermal.valid ? 1 : 0;
                                batch_capacitor_hotspot_temps.push_back(capacitor_thermal.capacitor_hotspot_temperature);
                                
                                // Get capacitor voltage based on type
                                if (cap_coeffs_loaded) {
                                    if (cap_voltage_type == "AC") {
                                        batch_capacitor_voltages.push_back(sc.ac_voltage);
                                    } else {
                                        // For DC capacitors: in 3-level topology, each capacitor sees half the total DC-link voltage
                                        double dc_voltage = params.vdc_target;
                                        if (params.topology_level == 3) {
                                            dc_voltage = params.vdc_target / 2.0;
                                        }
                                        batch_capacitor_voltages.push_back(dc_voltage);
                                    }
                                } else {
                                    batch_capacitor_voltages.push_back(0.0);
                                }
                                
                                batch_junction_temps.push_back(power_module_thermal.junction_temperature);
                            }
                            thread_profile.stress_loss_thermal +=
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - stress_loss_thermal_start).count();
                                
                                // ====================================================================
                            // Section 5: Reliability Analysis - Degradation Tracking (Batch Processing on GPU)
                                // ====================================================================
                                
                            // 5.1 Cooling Fan Reliability: Calculate 4 stressors on GPU
                            // 4 situations: internal electrical, internal mechanical, ambient electrical, ambient mechanical
                            const auto fan_reliability_start = std::chrono::steady_clock::now();
                            if (!sim_model.fan_cooling_part_number.empty() && !batch_ambient_temps.empty()) {
                                    FanCoefficients fan_coeffs;
                                    if (component_db.load_fan_cooling(sim_model.fan_cooling_part_number, fan_coeffs)) {
                                    double fan_stressors[4] = {0.0, 0.0, 0.0, 0.0};  // [internal_electrical, internal_mechanical, ambient_electrical, ambient_mechanical]
                                    calculate_fan_reliability_stressors_gpu(
                                        batch_ambient_temps.data(),
                                        batch_ambient_rhs.data(),
                                        batch_internal_temps.data(),
                                        batch_internal_rhs.data(),
                                        static_cast<int>(batch_ambient_temps.size()),
                                        fan_coeffs,
                                        fan_stressors
                                    );
                                    
                                    fan_stressor_electrical_internal += fan_stressors[0];
                                    fan_stressor_mechanical_internal += fan_stressors[1];
                                    fan_stressor_electrical_external += fan_stressors[2];
                                    fan_stressor_mechanical_external += fan_stressors[3];
                                    
                                    // Track per-case fan stressors (distribute batch stressor equally across cases)
                                    double per_case_fan_electrical_internal = fan_stressors[0] / batch_ambient_temps.size();
                                    double per_case_fan_mechanical_internal = fan_stressors[1] / batch_ambient_temps.size();
                                    double per_case_fan_electrical_external = fan_stressors[2] / batch_ambient_temps.size();
                                    double per_case_fan_mechanical_external = fan_stressors[3] / batch_ambient_temps.size();
                                    
                                    #pragma omp critical
                                    {
                                        for (size_t i = 0; i < batch_ambient_temps.size(); ++i) {
                                            int actual_case_idx = batch_start + static_cast<int>(i);
                                            CaseStressorRecord record;
                                            record.case_index = actual_case_idx;
                                            record.fan_electrical_external = per_case_fan_electrical_external;
                                            record.fan_electrical_internal = per_case_fan_electrical_internal;
                                            record.fan_mechanical_external = per_case_fan_mechanical_external;
                                            record.fan_mechanical_internal = per_case_fan_mechanical_internal;
                                            record.capacitor = 0.0;
                                            record.igbt_deltaT = 0.0;
                                            record.igbt_arrhenius = 0.0;
                                            record.pcb = 0.0;
                                            round_stressor_records.push_back(record);
                                        }
                                    }
                                    }
                                }
                            thread_profile.fan_reliability +=
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - fan_reliability_start).count();
                                
                                // 5.2 IGBT Reliability: Accumulate junction temperatures for rainflow counting
                            accumulated_junction_temps.insert(accumulated_junction_temps.end(), 
                                                             batch_junction_temps.begin(), batch_junction_temps.end());
                            // Track case indices for junction temps
                            // Also track RH and voltage for Arrhenius model (corrosion/dendrites)
                            for (size_t i = 0; i < batch_junction_temps.size(); ++i) {
                                int actual_case_idx = batch_start + static_cast<int>(i);
                                accumulated_junction_case_indices.push_back(actual_case_idx);
                                // Get corresponding RH and voltage for this case
                                if (i < batch_internal_rhs.size()) {
                                    accumulated_junction_rhs.push_back(batch_internal_rhs[i]);
                                } else {
                                    accumulated_junction_rhs.push_back(50.0);  // Default RH if not available
                                }
                                if (i < batch_ac_voltages.size()) {
                                    accumulated_junction_voltages.push_back(batch_ac_voltages[i]);
                                } else {
                                    accumulated_junction_voltages.push_back(400.0);  // Default voltage if not available
                                }
                            }
                                
                            // 5.3 Capacitor Reliability: Calculate stressor on GPU
                            const auto capacitor_reliability_start = std::chrono::steady_clock::now();
                            if (!sim_model.capacitor_part_number.empty() && !batch_capacitor_hotspot_temps.empty()) {
                                    CapacitorCoefficients cap_coeffs;
                                    CapacitorType cap_type;
                                std::string cap_voltage_type;
                                
                                // Debug: Check if capacitor loads
                                bool cap_loaded = component_db.load_capacitor(sim_model.capacitor_part_number, cap_coeffs, cap_type, cap_voltage_type);
                                if (!cap_loaded) {
                                    std::cerr << "DEBUG: Failed to load capacitor: " << sim_model.capacitor_part_number << std::endl;
                                } else {
                                    cap_coeffs.type = cap_type;
                                    
                                    // Debug: Print capacitor info
                                    static bool debug_printed = false;
                                    if (!debug_printed && batch_capacitor_hotspot_temps.size() > 0) {
                                        std::cerr << "DEBUG: Capacitor loaded successfully:" << std::endl;
                                        std::cerr << "  Part number: " << sim_model.capacitor_part_number << std::endl;
                                        std::cerr << "  Type: " << (cap_type == CapacitorType::FILM ? "FILM" : 
                                                                    cap_type == CapacitorType::ALUMINUM_ELECTROLYTIC ? "ALUMINUM_ELECTROLYTIC" : "OTHER") << std::endl;
                                        std::cerr << "  Voltage type: " << cap_voltage_type << std::endl;
                                        std::cerr << "  Batch size: " << batch_capacitor_hotspot_temps.size() << std::endl;
                                        if (batch_capacitor_voltages.size() > 0) {
                                            std::cerr << "  First voltage: " << batch_capacitor_voltages[0] << " V" << std::endl;
                                        }
                                        std::cerr << "  First hotspot temp: " << batch_capacitor_hotspot_temps[0] << " C" << std::endl;
                                        if (batch_internal_rhs.size() > 0) {
                                            std::cerr << "  First internal RH: " << batch_internal_rhs[0] << " %" << std::endl;
                                        }
                                        if (cap_type == CapacitorType::ALUMINUM_ELECTROLYTIC) {
                                            std::cerr << "  L0: " << cap_coeffs.coeffs.aluminum.L0 << std::endl;
                                            std::cerr << "  V0: " << cap_coeffs.coeffs.aluminum.V0 << std::endl;
                                            std::cerr << "  T0: " << cap_coeffs.coeffs.aluminum.T0 << std::endl;
                                            std::cerr << "  beta_min: " << cap_coeffs.coeffs.aluminum.beta_min << std::endl;
                                            std::cerr << "  beta_max: " << cap_coeffs.coeffs.aluminum.beta_max << std::endl;
                                        }
                                    }
                                    
                                    double batch_capacitor_stressor = 0.0;
                                    calculate_capacitor_reliability_stressor_gpu(
                                        batch_capacitor_voltages.data(),
                                        batch_capacitor_hotspot_temps.data(),
                                        batch_internal_rhs.data(),
                                        static_cast<int>(batch_capacitor_hotspot_temps.size()),
                                            cap_type,
                                        cap_coeffs,
                                        batch_capacitor_stressor
                                        );
                                    
                                    // Debug: Print stressor result
                                    if (!debug_printed) {
                                        std::cerr << "  Batch capacitor stressor: " << batch_capacitor_stressor << std::endl;
                                        debug_printed = true;
                                    }
                                    
                                    capacitor_stressor += batch_capacitor_stressor;
                                    
                                    // Track per-case capacitor stressors (distribute batch stressor equally across cases)
                                    double per_case_capacitor_stressor = batch_capacitor_stressor / batch_capacitor_hotspot_temps.size();
                                    
                                    #pragma omp critical
                                    {
                                        for (size_t i = 0; i < batch_capacitor_hotspot_temps.size(); ++i) {
                                            int actual_case_idx = batch_start + static_cast<int>(i);
                                            // Find or create record for this case
                                            auto it = std::find_if(round_stressor_records.begin(), round_stressor_records.end(),
                                                [actual_case_idx](const CaseStressorRecord& r) { return r.case_index == actual_case_idx; });
                                            if (it != round_stressor_records.end()) {
                                                it->capacitor = per_case_capacitor_stressor;
                                            } else {
                                                CaseStressorRecord record;
                                                record.case_index = actual_case_idx;
                                                record.fan_electrical_external = 0.0;
                                                record.fan_electrical_internal = 0.0;
                                                record.fan_mechanical_external = 0.0;
                                                record.fan_mechanical_internal = 0.0;
                                                record.capacitor = per_case_capacitor_stressor;
                                                record.igbt_deltaT = 0.0;
                                                record.igbt_arrhenius = 0.0;
                                                record.pcb = 0.0;
                                                round_stressor_records.push_back(record);
                                            }
                                        }
                                    }
                                }
                            } else {
                                // Debug: Why capacitor calculation is skipped
                                static bool skip_debug_printed = false;
                                if (!skip_debug_printed) {
                                    if (sim_model.capacitor_part_number.empty()) {
                                        std::cerr << "DEBUG: Capacitor calculation skipped - part number is empty" << std::endl;
                                    }
                                    if (batch_capacitor_hotspot_temps.empty()) {
                                        std::cerr << "DEBUG: Capacitor calculation skipped - batch_capacitor_hotspot_temps is empty" << std::endl;
                                    }
                                    skip_debug_printed = true;
                                }
                            }
                            thread_profile.capacitor_reliability +=
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - capacitor_reliability_start).count();
                                
                                // Collect internal temperatures for PCB reliability analysis and IGBT Arrhenius model
                                for (size_t case_idx = 0; case_idx < batch_internal_temps.size(); ++case_idx) {
                                    int actual_case_idx = batch_start + static_cast<int>(case_idx);
                                    accumulated_internal_temps.push_back(batch_internal_temps[case_idx]);
                                    accumulated_internal_case_indices.push_back(actual_case_idx);
                                    // Track RH and voltage for Arrhenius model (corrosion/dendrites)
                                    if (case_idx < batch_internal_rhs.size()) {
                                        accumulated_internal_rhs.push_back(batch_internal_rhs[case_idx]);
                                    } else {
                                        accumulated_internal_rhs.push_back(50.0);  // Default RH if not available
                                    }
                                    if (case_idx < batch_ac_voltages.size()) {
                                        accumulated_internal_voltages.push_back(batch_ac_voltages[case_idx]);
                                    } else {
                                        accumulated_internal_voltages.push_back(400.0);  // Default voltage if not available
                                    }
                                }
                            
                            thread_simulation_time += batch_outputs.elapsed_s;
                            thread_cases_processed += static_cast<int>(params_batch.size());
                            #pragma omp critical(tracepv_progress_output)
                            {
                                cases_reported_in_round += static_cast<int>(params_batch.size());
                                std::cout << "TRACEPV_PROGRESS iteration=" << mission_profile_iteration
                                          << " round=" << (round + 1) << "/" << options.num_rounds
                                          << " pass=" << round_attempt
                                          << " processed=" << std::min(cases_reported_in_round, round_cases)
                                          << " total=" << round_cases << std::endl;
                            }
                            for (int loaded_idx : loaded_cache_indices) {
                                if (loaded_idx >= 0 &&
                                    loaded_idx < static_cast<int>(stored_electrical_results.size())) {
                                    release_stored_reference_payload(
                                        stored_electrical_results[static_cast<std::size_t>(loaded_idx)]);
                                }
                            }
                            release_batch_memory(batch_outputs, params_batch);
                        }
                        
                        // Perform rainflow counting and reliability analysis for IGBT and PCB
                        // after accumulating temperature data across all batches in this thread
                        
                        // 5.2 IGBT Reliability: Two failure mechanisms
                        // 5.2.1 DeltaT model: Rainflow counting on junction temperatures
                        const auto igbt_reliability_start = std::chrono::steady_clock::now();
                        if (!accumulated_junction_temps.empty() && !sim_model.power_module_part_number.empty()) {
                            PowerModuleCoefficients pm_coeffs;
                            if (component_db.load_power_module(sim_model.power_module_part_number, pm_coeffs)) {
                                if (reference_post_processing) {
                                // Debug: Print IGBT junction temperature statistics
                                static bool igbt_rainflow_debug_printed = false;
                                if (!igbt_rainflow_debug_printed) {
                                    std::cerr << "DEBUG: IGBT Reliability Analysis:" << std::endl;
                                    std::cerr << "  Accumulated junction temps: " << accumulated_junction_temps.size() << " points" << std::endl;
                                    if (!accumulated_junction_temps.empty()) {
                                        auto minmax = std::minmax_element(accumulated_junction_temps.begin(), accumulated_junction_temps.end());
                                        std::cerr << "  Junction temp range: [" << *minmax.first << ", " << *minmax.second << "] C" << std::endl;
                                    }
                                    igbt_rainflow_debug_printed = true;
                                }
                                
                                // Perform rainflow counting with threshold (e.g., 1°C)
                                const double rainflow_threshold = 1.0;  // Minimum temperature difference to consider
                                RainflowResult rainflow_result = rainflow_counting(
                                    accumulated_junction_temps,
                                    rainflow_threshold
                                );
                                
                                // Debug: Print rainflow counting results
                                static bool igbt_rainflow_result_debug_printed = false;
                                if (!igbt_rainflow_result_debug_printed) {
                                    std::cerr << "DEBUG: IGBT Rainflow Counting Results:" << std::endl;
                                    std::cerr << "  Tj_max: " << rainflow_result.Tj_max << " C" << std::endl;
                                    std::cerr << "  Number of cycles: " << rainflow_result.delta_range.size() << std::endl;
                                    if (!rainflow_result.delta_range.empty()) {
                                        std::cerr << "  First few cycles:" << std::endl;
                                        size_t num_to_print = std::min(size_t(5), rainflow_result.delta_range.size());
                                        for (size_t i = 0; i < num_to_print; ++i) {
                                            std::cerr << "    Cycle " << i << ": deltaT=" << rainflow_result.delta_range[i] 
                                                      << " C, cycles=" << rainflow_result.delta_cycle[i] << std::endl;
                                        }
                                    }
                                    igbt_rainflow_result_debug_printed = true;
                                }
                                
                                // DeltaT model: Calculate stressor = sum of 1/Nf for each deltaT
                                static bool igbt_deltat_debug_printed = false;
                                double total_deltaT_stressor = 0.0;
                                std::map<int, double> case_igbt_deltaT_stressor;
                                
                                if (!rainflow_result.delta_range.empty() && 
                                    !rainflow_result.delta_cycle.empty() &&
                                    !rainflow_result.begin_idx.empty() &&
                                    !rainflow_result.end_idx.empty() &&
                                    rainflow_result.delta_range.size() == rainflow_result.delta_cycle.size() &&
                                    rainflow_result.delta_range.size() == rainflow_result.begin_idx.size() &&
                                    rainflow_result.delta_range.size() == rainflow_result.end_idx.size()) {
                                    
                                    for (size_t i = 0; i < rainflow_result.delta_range.size(); ++i) {
                                        double deltaT = rainflow_result.delta_range[i];
                                        double cycles = rainflow_result.delta_cycle[i];
                                        
                                        // Get Tj_max: compare temperature at begin_idx and end_idx, use the larger one
                                        size_t begin_idx = rainflow_result.begin_idx[i];
                                        size_t end_idx = rainflow_result.end_idx[i];
                                        if (begin_idx < accumulated_junction_temps.size() && 
                                            end_idx < accumulated_junction_temps.size()) {
                                            double Tj_begin = accumulated_junction_temps[begin_idx];
                                            double Tj_end = accumulated_junction_temps[end_idx];
                                            double Tj_max = std::max(Tj_begin, Tj_end);
                                            
                                            // Calculate Nf for this temperature cycle using deltaT model
                                            double Nf = calculate_igbt_nf_deltaT(deltaT, Tj_max, pm_coeffs);
                                            
                                            // Stressor per cycle = 1/Nf, accumulate for all cycles
                                            if (Nf > 0.0) {
                                                double cycle_stressor = cycles / Nf;
                                                igbt_stressor_deltaT += cycle_stressor;
                                                total_deltaT_stressor += cycle_stressor;
                                                
                                                // Debug: Print first few deltaT calculations
                                                if (!igbt_deltat_debug_printed && i < 5) {
                                                    std::cerr << "DEBUG: IGBT DeltaT Model Calculation (Cycle " << i << "):" << std::endl;
                                                    std::cerr << "  deltaT: " << deltaT << " C" << std::endl;
                                                    std::cerr << "  Tj_begin: " << Tj_begin << " C" << std::endl;
                                                    std::cerr << "  Tj_end: " << Tj_end << " C" << std::endl;
                                                    std::cerr << "  Tj_max: " << Tj_max << " C" << std::endl;
                                                    std::cerr << "  cycles: " << cycles << std::endl;
                                                    std::cerr << "  Nf: " << Nf << std::endl;
                                                    std::cerr << "  stressor (cycles/Nf): " << cycle_stressor << std::endl;
                                                    if (i == 4) igbt_deltat_debug_printed = true;
                                                }
                                            }
                                        }
                                    }
                                    
                                    if (!igbt_deltat_debug_printed) {
                                        std::cerr << "DEBUG: IGBT DeltaT Model Total Stressor: " << total_deltaT_stressor << std::endl;
                                        igbt_deltat_debug_printed = true;
                                    }
                                    
                                    // Distribute IGBT DeltaT stressor across cases proportionally
                                    // Each case contributes based on how many of its junction temps are in cycles
                                    if (total_deltaT_stressor > 0.0 && !accumulated_junction_case_indices.empty()) {
                                        // Count how many times each case appears in cycles
                                        std::map<int, int> case_cycle_count;
                                        for (size_t i = 0; i < rainflow_result.delta_range.size(); ++i) {
                                            size_t begin_idx = rainflow_result.begin_idx[i];
                                            size_t end_idx = rainflow_result.end_idx[i];
                                            if (begin_idx < accumulated_junction_case_indices.size()) {
                                                int case_idx = accumulated_junction_case_indices[begin_idx];
                                                case_cycle_count[case_idx]++;
                                            }
                                            if (end_idx < accumulated_junction_case_indices.size()) {
                                                int case_idx = accumulated_junction_case_indices[end_idx];
                                                case_cycle_count[case_idx]++;
                                            }
                                        }
                                        
                                        // Distribute stressor proportionally
                                        int total_cycle_appearances = 0;
                                        for (const auto& pair : case_cycle_count) {
                                            total_cycle_appearances += pair.second;
                                        }
                                        
                                        if (total_cycle_appearances > 0) {
                                            for (const auto& pair : case_cycle_count) {
                                                double proportion = static_cast<double>(pair.second) / total_cycle_appearances;
                                                case_igbt_deltaT_stressor[pair.first] = total_deltaT_stressor * proportion;
                                            }
                                        }
                                    }
                                    
                                    // Update records with IGBT DeltaT stressors
                                    #pragma omp critical
                                    {
                                        for (const auto& pair : case_igbt_deltaT_stressor) {
                                            int case_idx = pair.first;
                                            auto it = std::find_if(round_stressor_records.begin(), round_stressor_records.end(),
                                                [case_idx](const CaseStressorRecord& r) { return r.case_index == case_idx; });
                                            if (it != round_stressor_records.end()) {
                                                it->igbt_deltaT = pair.second;
                                            } else {
                                                CaseStressorRecord record;
                                                record.case_index = case_idx;
                                                record.fan_electrical_external = 0.0;
                                                record.fan_electrical_internal = 0.0;
                                                record.fan_mechanical_external = 0.0;
                                                record.fan_mechanical_internal = 0.0;
                                                record.capacitor = 0.0;
                                                record.igbt_deltaT = pair.second;
                                                record.igbt_arrhenius = 0.0;
                                                record.pcb = 0.0;
                                                round_stressor_records.push_back(record);
                                            }
                                        }
                                    }
                                }
                                }
                                
                                // 5.2.2 Arrhenius model: Calculate stressor from internal temperatures
                                // For each 5-minute interval, calculate lifetime and stressor
                                // Stressor = (1 * 5 min) / (lifetime * 60) = 5 / (lifetime * 60)
                                const double interval_minutes = 5.0;
                                static bool igbt_arrhenius_debug_printed = false;
                                double total_arrhenius_stressor = 0.0;
                                
                                // Track per-case IGBT Arrhenius stressors
                                std::map<int, double> case_igbt_arrhenius_stressor;
                                
                                for (size_t i = 0; i < accumulated_internal_temps.size(); ++i) {
                                    double T_internal = accumulated_internal_temps[i];
                                    int case_idx = (i < accumulated_internal_case_indices.size()) ? 
                                        accumulated_internal_case_indices[i] : -1;
                                    // Get corresponding RH and voltage for Arrhenius model (corrosion/dendrites)
                                    double rh = (i < accumulated_internal_rhs.size()) ? accumulated_internal_rhs[i] : 50.0;
                                    double voltage = (i < accumulated_internal_voltages.size()) ? accumulated_internal_voltages[i] : 400.0;
                                    double lifetime = calculate_igbt_lifetime_arrhenius(T_internal, rh, voltage, pm_coeffs);
                                    
                                    // Stressor = 5 min / (lifetime * 60 min/hour)
                                    // Convert lifetime from hours to minutes: lifetime * 60
                                    if (lifetime > 0.0) {
                                        double interval_stressor = interval_minutes / (lifetime * 60.0);
                                        igbt_stressor_arrhenius += interval_stressor;
                                        total_arrhenius_stressor += interval_stressor;
                                        
                                        // Track per-case Arrhenius stressor
                                        if (case_idx >= 0) {
                                            case_igbt_arrhenius_stressor[case_idx] += interval_stressor;
                                        }
                                        
                                        // Debug: Print first few Arrhenius calculations
                                        if (!igbt_arrhenius_debug_printed && i < 5) {
                                            std::cerr << "DEBUG: IGBT Arrhenius Model Calculation (Interval " << i << "):" << std::endl;
                                            std::cerr << "  T_internal: " << T_internal << " C" << std::endl;
                                            std::cerr << "  RH: " << rh << " %" << std::endl;
                                            std::cerr << "  Voltage: " << voltage << " V" << std::endl;
                                            std::cerr << "  Lifetime: " << lifetime << " hours" << std::endl;
                                            std::cerr << "  Stressor (5 min / lifetime*60): " << interval_stressor << std::endl;
                                            if (i == 4) igbt_arrhenius_debug_printed = true;
                                        }
                                    }
                                }
                                
                                // Update records with IGBT Arrhenius stressors
                                #pragma omp critical
                                {
                                    for (const auto& pair : case_igbt_arrhenius_stressor) {
                                        int case_idx = pair.first;
                                        auto it = std::find_if(round_stressor_records.begin(), round_stressor_records.end(),
                                            [case_idx](const CaseStressorRecord& r) { return r.case_index == case_idx; });
                                        if (it != round_stressor_records.end()) {
                                            it->igbt_arrhenius = pair.second;
                                        } else {
                                            CaseStressorRecord record;
                                            record.case_index = case_idx;
                                            record.fan_electrical_external = 0.0;
                                            record.fan_electrical_internal = 0.0;
                                            record.fan_mechanical_external = 0.0;
                                            record.fan_mechanical_internal = 0.0;
                                            record.capacitor = 0.0;
                                            record.igbt_deltaT = 0.0;
                                            record.igbt_arrhenius = pair.second;
                                            record.pcb = 0.0;
                                            round_stressor_records.push_back(record);
                                        }
                                    }
                                }
                                
                                if (!igbt_arrhenius_debug_printed) {
                                    std::cerr << "DEBUG: IGBT Arrhenius Model Total Stressor: " << total_arrhenius_stressor << std::endl;
                                    std::cerr << "DEBUG: IGBT Arrhenius Model - Processed " << accumulated_internal_temps.size() << " intervals" << std::endl;
                                    igbt_arrhenius_debug_printed = true;
                                }
                            } else {
                                static bool igbt_load_error_printed = false;
                                if (!igbt_load_error_printed) {
                                    std::cerr << "DEBUG: Failed to load IGBT power module: " << sim_model.power_module_part_number << std::endl;
                                    igbt_load_error_printed = true;
                                }
                            }
                        }
                        thread_profile.igbt_reliability +=
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - igbt_reliability_start).count();
                        
                        // 5.4 PCB Reliability: Rainflow counting on internal temperatures
                        const auto pcb_reliability_start = std::chrono::steady_clock::now();
                        if (reference_post_processing &&
                            !accumulated_internal_temps.empty() && !sim_model.pcb_part_number.empty()) {
                            PCBParameters pcb_params;
                            if (component_db.load_pcb(sim_model.pcb_part_number, pcb_params)) {
                                // Debug: Print PCB parameters
                                static bool pcb_params_debug_printed = false;
                                if (!pcb_params_debug_printed) {
                                    std::cerr << "DEBUG: PCB Reliability Analysis:" << std::endl;
                                    std::cerr << "  Part number: " << sim_model.pcb_part_number << std::endl;
                                    std::cerr << "  Component type: " << pcb_params.component_type << std::endl;
                                    std::cerr << "  Material: " << pcb_params.material << std::endl;
                                    std::cerr << "  Component dimensions (l x w x h): " << pcb_params.component_dims.length 
                                              << " x " << pcb_params.component_dims.width 
                                              << " x " << pcb_params.component_dims.thickness << " mm" << std::endl;
                                    std::cerr << "  Copper dimensions (l x w x h): " << pcb_params.copper_dims.length 
                                              << " x " << pcb_params.copper_dims.width 
                                              << " x " << pcb_params.copper_dims.thickness << " mm" << std::endl;
                                    std::cerr << "  Solder dimensions (l x w x h): " << pcb_params.solder_dims.length 
                                              << " x " << pcb_params.solder_dims.width 
                                              << " x " << pcb_params.solder_dims.thickness << " mm" << std::endl;
                                    std::cerr << "  CTE_component: " << std::scientific << pcb_params.CTE_component << " 1/K" << std::endl;
                                    std::cerr << "  CTE_FR4: " << std::scientific << pcb_params.CTE_FR4 << " 1/K" << std::endl;
                                    std::cerr << "  d_CTE: " << std::scientific << (pcb_params.CTE_component - pcb_params.CTE_FR4) << " 1/K" << std::endl;
                                    std::cerr << "  PCB thickness: " << std::fixed << pcb_params.pcb_thickness << " mm" << std::endl;
                                    std::cerr << "  E_comp: " << std::fixed << 310000.0 << " Pa" << std::endl;
                                    std::cerr << "  E_FR4: " << std::fixed << pcb_params.E_FR4 << " Pa" << std::endl;
                                    std::cerr << "  G_solder: " << std::fixed << pcb_params.shear_modulus << " Pa" << std::endl;
                                    std::cerr << "  G_copper: " << std::fixed << pcb_params.G_copper << " Pa" << std::endl;
                                    std::cerr << "  G_FR4: " << std::fixed << pcb_params.G_FR4 << " Pa" << std::endl;
                                    std::cerr << "  Poisson_FR4: " << std::fixed << pcb_params.Poisson_FR4 << std::endl;
                                    std::cerr << "  Adjust param: " << std::fixed << pcb_params.adjust_param << std::endl;
                                    std::cerr << "  Accumulated internal temps: " << accumulated_internal_temps.size() << " points" << std::endl;
                                    if (!accumulated_internal_temps.empty()) {
                                        auto minmax = std::minmax_element(accumulated_internal_temps.begin(), accumulated_internal_temps.end());
                                        std::cerr << "  Internal temp range: [" << std::fixed << *minmax.first << ", " << *minmax.second << "] C" << std::endl;
                                    }
                                    pcb_params_debug_printed = true;
                                }
                                
                                // Perform rainflow counting with threshold (e.g., 1°C)
                                const double rainflow_threshold = 1.0;  // Minimum temperature difference to consider
                                RainflowResult rainflow_result = rainflow_counting(
                                    accumulated_internal_temps,
                                    rainflow_threshold
                                );
                                
                                // Debug: Print rainflow counting results
                                static bool pcb_rainflow_result_debug_printed = false;
                                if (!pcb_rainflow_result_debug_printed) {
                                    std::cerr << "DEBUG: PCB Rainflow Counting Results:" << std::endl;
                                    std::cerr << "  Number of cycles: " << rainflow_result.delta_range.size() << std::endl;
                                    if (!rainflow_result.delta_range.empty()) {
                                        auto minmax_delta = std::minmax_element(rainflow_result.delta_range.begin(), rainflow_result.delta_range.end());
                                        std::cerr << "  Delta T range: [" << *minmax_delta.first << ", " << *minmax_delta.second << "] C" << std::endl;
                                        std::cerr << "  First few cycles:" << std::endl;
                                        size_t num_to_print = std::min(size_t(5), rainflow_result.delta_range.size());
                                        for (size_t i = 0; i < num_to_print; ++i) {
                                            std::cerr << "    Cycle " << i << ": deltaT=" << rainflow_result.delta_range[i] 
                                                      << " C, cycles=" << rainflow_result.delta_cycle[i] << std::endl;
                                        }
                                    }
                                    pcb_rainflow_result_debug_printed = true;
                                }
                                
                                // Calculate stressor using new model: sum of cycles/Nf for each deltaT
                                // Also calculate detailed debug info for first few cycles
                                static bool pcb_stressor_debug_printed = false;
                                double total_pcb_stressor = 0.0;
                                
                                // Calculate Nf and stressor for each cycle with debug output
                                for (size_t i = 0; i < rainflow_result.delta_range.size(); ++i) {
                                    double delta_T = rainflow_result.delta_range[i];
                                    double cycles = rainflow_result.delta_cycle[i];
                                    
                                    // Calculate Nf for this delta_T using PCB reliability model
                                    double Nf = calculate_pcb_nf(delta_T, pcb_params);
                                    
                                    // Accumulate stressor: cycles/Nf for each cycle
                                    if (Nf > 0.0) {
                                        double cycle_stressor = cycles / Nf;
                                        total_pcb_stressor += cycle_stressor;
                                        
                                        // Debug: Print first few PCB calculations with detailed intermediate values
                                        if (!pcb_stressor_debug_printed && i < 5) {
                                            std::cerr << "DEBUG: PCB Stressor Calculation (Cycle " << i << "):" << std::endl;
                                            std::cerr << "  deltaT: " << std::fixed << std::setprecision(3) << delta_T << " C" << std::endl;
                                            std::cerr << "  cycles: " << std::fixed << cycles << std::endl;
                                            
                                            // Calculate intermediate values for debug output (duplicate calculation for debugging)
                                            double delta_T_abs = std::abs(delta_T);
                                            double component_l = pcb_params.component_dims.length;
                                            double component_w = pcb_params.component_dims.width;
                                            double component_h = pcb_params.component_dims.thickness;
                                            double copper_l = pcb_params.copper_dims.length;
                                            double copper_w = pcb_params.copper_dims.width;
                                            double copper_h = pcb_params.copper_dims.thickness;
                                            double solder_l = pcb_params.solder_dims.length;
                                            double solder_w = pcb_params.solder_dims.width;
                                            double solder_h = pcb_params.solder_dims.thickness;
                                            double CTE_comp = pcb_params.CTE_component;
                                            double CTE_FR4 = pcb_params.CTE_FR4;
                                            double E_comp = 310000.0;
                                            double E_FR4 = pcb_params.E_FR4;
                                            double G_solder = pcb_params.shear_modulus;
                                            double G_copper = pcb_params.G_copper;
                                            double G_FR4 = pcb_params.G_FR4;
                                            double Poisson_FR4 = pcb_params.Poisson_FR4;
                                            double pcb_thickness = pcb_params.pcb_thickness;
                                            double d_CTE = CTE_comp - CTE_FR4;
                                            double Ld_SE = 0.5 * component_l;
                                            double A_SE = solder_l * solder_w;
                                            double A1_SE = component_h * component_w;
                                            double A2_SE = 2.0 * copper_w * pcb_thickness;
                                            double Ab_SE = copper_l * copper_w;
                                            double hb_SE = copper_h;
                                            double As_SE = 0.75 * Ab_SE;
                                            double a_SE = 0.5 * copper_l;
                                            double hs_SE = solder_h;
                                            double d_strain_SE = 1.38271485223961 * std::sqrt(Ld_SE * Ld_SE * Ld_SE / (A_SE * hs_SE)) * std::abs(d_CTE * delta_T_abs);
                                            double Fnum_SE = (CTE_FR4 - CTE_comp) * delta_T_abs * Ld_SE;
                                            double Fden_SE = Ld_SE / (E_comp * A1_SE) + Ld_SE / (E_FR4 * A2_SE) + hs_SE / (As_SE * G_solder) + hb_SE / (Ab_SE * G_copper) + (2.0 - Poisson_FR4) / (9.0 * G_FR4 * a_SE);
                                            double F = Fnum_SE / Fden_SE;
                                            double Tau_SE = F / As_SE;
                                            double dW_SE = Tau_SE * d_strain_SE;
                                            double adjust_param = pcb_params.adjust_param;
                                            
                                            std::cerr << "  Ld_SE: " << std::scientific << std::setprecision(6) << Ld_SE << " mm" << std::endl;
                                            std::cerr << "  A_SE: " << std::scientific << A_SE << " mm^2" << std::endl;
                                            std::cerr << "  A1_SE: " << std::scientific << A1_SE << " mm^2" << std::endl;
                                            std::cerr << "  A2_SE: " << std::scientific << A2_SE << " mm^2" << std::endl;
                                            std::cerr << "  As_SE: " << std::scientific << As_SE << " mm^2" << std::endl;
                                            std::cerr << "  d_strain_SE: " << std::scientific << d_strain_SE << std::endl;
                                            std::cerr << "  Fnum_SE: " << std::scientific << Fnum_SE << " N" << std::endl;
                                            std::cerr << "  Fden_SE: " << std::scientific << Fden_SE << " m/N" << std::endl;
                                            std::cerr << "  F: " << std::scientific << F << " N" << std::endl;
                                            std::cerr << "  Tau_SE: " << std::scientific << Tau_SE << " Pa" << std::endl;
                                            std::cerr << "  dW_SE: " << std::scientific << dW_SE << " J/m^3" << std::endl;
                                            std::cerr << "  adjust_param * dW_SE / 5920: " << std::scientific << (adjust_param * dW_SE / 5920.0) << std::endl;
                                            std::cerr << "  Nf: " << std::scientific << Nf << " cycles to failure" << std::endl;
                                            std::cerr << "  stressor (cycles/Nf): " << std::scientific << cycle_stressor << std::endl;
                                            if (i == 4) pcb_stressor_debug_printed = true;
                                        }
                                    }
                                }
                                
                                if (!pcb_stressor_debug_printed) {
                                    std::cerr << "DEBUG: PCB Total Stressor: " << total_pcb_stressor << std::endl;
                                    pcb_stressor_debug_printed = true;
                                }
                                
                                pcb_degradation = total_pcb_stressor;
                                
                                // Distribute PCB stressor to cases (simplified: distribute equally)
                                double per_case_pcb_stressor = pcb_degradation / accumulated_internal_temps.size();
                                
                                // Update records with PCB stressors
                                #pragma omp critical
                                {
                                    for (int case_idx : accumulated_internal_case_indices) {
                                        auto it = std::find_if(round_stressor_records.begin(), round_stressor_records.end(),
                                            [case_idx](const CaseStressorRecord& r) { return r.case_index == case_idx; });
                                        if (it != round_stressor_records.end()) {
                                            it->pcb = per_case_pcb_stressor;
                                        } else {
                                            CaseStressorRecord record;
                                            record.case_index = case_idx;
                                            record.fan_electrical_external = 0.0;
                                            record.fan_electrical_internal = 0.0;
                                            record.fan_mechanical_external = 0.0;
                                            record.fan_mechanical_internal = 0.0;
                                            record.capacitor = 0.0;
                                            record.igbt_deltaT = 0.0;
                                            record.igbt_arrhenius = 0.0;
                                            record.pcb = per_case_pcb_stressor;
                                            round_stressor_records.push_back(record);
                                        }
                                    }
                                }
                            } else {
                                // Debug: Why PCB calculation is skipped
                                static bool pcb_skip_debug_printed = false;
                                if (!pcb_skip_debug_printed) {
                                    if (sim_model.pcb_part_number.empty()) {
                                        std::cerr << "DEBUG: PCB calculation skipped - part number is empty" << std::endl;
                                    } else if (accumulated_internal_temps.empty()) {
                                        std::cerr << "DEBUG: PCB calculation skipped - accumulated_internal_temps is empty" << std::endl;
                                    } else {
                                        PCBParameters pcb_params_check;
                                        if (!component_db.load_pcb(sim_model.pcb_part_number, pcb_params_check)) {
                                            std::cerr << "DEBUG: PCB calculation skipped - failed to load PCB: " << sim_model.pcb_part_number << std::endl;
                                        }
                                    }
                                    pcb_skip_debug_printed = true;
                                }
                            }
                        }
                        thread_profile.pcb_reliability +=
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - pcb_reliability_start).count();
                        
                        // Accumulate results (thread-safe with critical section)
                        #pragma omp critical
                        {
                            iteration_simulation_time += thread_simulation_time;
                            iteration_gpu_simulation_times[cpu_thread_id] += thread_simulation_time;
                            cases_processed_in_round += thread_cases_processed;
                            total_cases_processed += thread_cases_processed;
                            profiling_totals.electrical_gpu += thread_profile.electrical_gpu;
                            profiling_totals.stress_loss_thermal += thread_profile.stress_loss_thermal;
                            profiling_totals.stress_calculation += thread_profile.stress_calculation;
                            profiling_totals.capacitor_loss += thread_profile.capacitor_loss;
                            profiling_totals.power_module_loss += thread_profile.power_module_loss;
                            profiling_totals.capacitor_thermal += thread_profile.capacitor_thermal;
                            profiling_totals.capacitor_ref_harmonic += thread_profile.capacitor_ref_harmonic;
                            profiling_totals.capacitor_ref_esr_grid += thread_profile.capacitor_ref_esr_grid;
                            profiling_totals.capacitor_ref_loss_grid += thread_profile.capacitor_ref_loss_grid;
                            profiling_totals.capacitor_ref_polyfit += thread_profile.capacitor_ref_polyfit;
                            profiling_totals.capacitor_ref_iteration += thread_profile.capacitor_ref_iteration;
                            profiling_totals.capacitor_fallback_static += thread_profile.capacitor_fallback_static;
                            profiling_totals.igbt_thermal += thread_profile.igbt_thermal;
                            profiling_totals.igbt_parameter_lookup += thread_profile.igbt_parameter_lookup;
                            profiling_totals.igbt_input_build += thread_profile.igbt_input_build;
                            profiling_totals.igbt_loss_igbt1 += thread_profile.igbt_loss_igbt1;
                            profiling_totals.igbt_loss_igbt2 += thread_profile.igbt_loss_igbt2;
                            profiling_totals.igbt_loss_diode1 += thread_profile.igbt_loss_diode1;
                            profiling_totals.igbt_loss_diode2 += thread_profile.igbt_loss_diode2;
                            profiling_totals.igbt_thermal_rc += thread_profile.igbt_thermal_rc;
                            profiling_totals.fan_reliability += thread_profile.fan_reliability;
                            profiling_totals.capacitor_reliability += thread_profile.capacitor_reliability;
                            profiling_totals.igbt_reliability += thread_profile.igbt_reliability;
                            profiling_totals.pcb_reliability += thread_profile.pcb_reliability;
                        }
                        
                        // Accumulate stressors from this thread (thread-safe)
                        #pragma omp critical
                        {
                            // Fan stressors
                            round_fan_stressor_electrical_external += fan_stressor_electrical_external;
                            round_fan_stressor_electrical_internal += fan_stressor_electrical_internal;
                            round_fan_stressor_mechanical_external += fan_stressor_mechanical_external;
                            round_fan_stressor_mechanical_internal += fan_stressor_mechanical_internal;
                            
                            // Capacitor stressor
                            round_capacitor_stressor += capacitor_stressor;
                            
                            // IGBT stressors (two models)
                            round_igbt_stressor_deltaT += igbt_stressor_deltaT;
                            round_igbt_stressor_arrhenius += igbt_stressor_arrhenius;
                            round_pcb_degradation += pcb_degradation;
                            
                            // Check if any accumulated stressor >= 1.0 (early termination)
                            if (!round_terminated_early && 
                                (round_fan_stressor_electrical_external >= 1.0 ||
                                 round_fan_stressor_electrical_internal >= 1.0 ||
                                 round_fan_stressor_mechanical_external >= 1.0 ||
                                 round_fan_stressor_mechanical_internal >= 1.0 ||
                                 round_capacitor_stressor >= 1.0 ||
                                 round_igbt_stressor_deltaT >= 1.0 ||
                                 round_igbt_stressor_arrhenius >= 1.0 ||
                                 round_pcb_degradation >= 1.0)) {
                                round_terminated_early = true;
                            }
                        }
                        
                        // Note: Early termination is handled in the loop condition above
                        // If round_terminated_early is set, the loop will exit naturally
                    } else {
                        std::cerr << "Failed to set device " << cpu_thread_id 
                                  << ": " << cudaGetErrorString(err) << std::endl;
                    }
                }
            }
            }

            if (!reuse_round_cache) {
                RoundStressorTotals cached_totals;
                cached_totals.fan_electrical_external = round_fan_stressor_electrical_external;
                cached_totals.fan_electrical_internal = round_fan_stressor_electrical_internal;
                cached_totals.fan_mechanical_external = round_fan_stressor_mechanical_external;
                cached_totals.fan_mechanical_internal = round_fan_stressor_mechanical_internal;
                cached_totals.capacitor = round_capacitor_stressor;
                cached_totals.igbt_deltaT = round_igbt_stressor_deltaT;
                cached_totals.igbt_arrhenius = round_igbt_stressor_arrhenius;
                cached_totals.pcb = round_pcb_degradation;
                round_cache.valid = true;
                round_cache.parameter_state = round_parameter_state;
                round_cache.totals = cached_totals;
                round_cache.records = round_stressor_records;
                round_cache.cases_processed = cases_processed_in_round;
            }
            
            // Check if any accumulated stressor >= 1.0 (early termination for round)
            if (round_fan_stressor_electrical_external >= 1.0 ||
                round_fan_stressor_electrical_internal >= 1.0 ||
                round_fan_stressor_mechanical_external >= 1.0 ||
                round_fan_stressor_mechanical_internal >= 1.0 ||
                round_capacitor_stressor >= 1.0 ||
                round_igbt_stressor_deltaT >= 1.0 ||
                round_igbt_stressor_arrhenius >= 1.0 ||
                round_pcb_degradation >= 1.0) {
                round_terminated_early = true;
                std::cout << "\nEarly termination: Accumulated stressor >= 1.0 detected in round " 
                          << (round + 1) << std::endl;
            }
            
            case_index += round_cases;
            
            // Write stressors to CSV file after each round
            const auto stressor_csv_start = std::chrono::steady_clock::now();
            int current_round = round;  // Capture round value to avoid potential naming conflicts
            std::string csv_filename = "results/stressor_round_" + std::to_string(current_round + 1) + 
                                      "_iteration_" + std::to_string(mission_profile_iteration) + ".csv";
            std::filesystem::path csv_path = std::filesystem::path("results");
            std::filesystem::create_directories(csv_path);
            
            std::ofstream csv_file(csv_filename);
            if (csv_file.is_open()) {
                // Write header
                csv_file << "case_index,fan_electrical_external,fan_electrical_internal,fan_mechanical_external,"
                         << "fan_mechanical_internal,capacitor,igbt_deltaT,igbt_arrhenius,pcb\n";
                
                // Sort records by case_index for consistent output
                std::sort(round_stressor_records.begin(), round_stressor_records.end(),
                    [](const CaseStressorRecord& a, const CaseStressorRecord& b) {
                        return a.case_index < b.case_index;
                    });
                
                // Write data
                for (const auto& record : round_stressor_records) {
                    csv_file << record.case_index << ","
                             << std::scientific << std::setprecision(15) << record.fan_electrical_external << ","
                             << record.fan_electrical_internal << ","
                             << record.fan_mechanical_external << ","
                             << record.fan_mechanical_internal << ","
                             << record.capacitor << ","
                             << record.igbt_deltaT << ","
                             << record.igbt_arrhenius << ","
                             << record.pcb << "\n";
                }
                
                csv_file.close();
                std::cout << "  Stressor data saved to: " << csv_filename << " (" 
                          << round_stressor_records.size() << " records)" << std::endl;
            } else {
                std::cerr << "  Warning: Failed to open CSV file for writing: " << csv_filename << std::endl;
            }
            profiling_totals.stressor_csv +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - stressor_csv_start).count();

            // Accumulate round stressors to iteration totals
            iteration_fan_stressor_electrical_external += round_fan_stressor_electrical_external;
            iteration_fan_stressor_electrical_internal += round_fan_stressor_electrical_internal;
            iteration_fan_stressor_mechanical_external += round_fan_stressor_mechanical_external;
            iteration_fan_stressor_mechanical_internal += round_fan_stressor_mechanical_internal;
            iteration_capacitor_stressor += round_capacitor_stressor;
            iteration_igbt_stressor_deltaT += round_igbt_stressor_deltaT;
            iteration_igbt_stressor_arrhenius += round_igbt_stressor_arrhenius;
            iteration_pcb_degradation += round_pcb_degradation;

            const DegradationParameterState after_round_parameter_state = current_parameter_state();
            if (parameter_state_exceeds(after_round_parameter_state, round_parameter_state)) {
                std::cout << "TRACEPV_CRITICAL_DEGRADATION iteration="
                          << mission_profile_iteration
                          << " round=" << (round + 1) << "/" << options.num_rounds
                          << " critical_step_percent="
                          << static_cast<int>(std::lround(kCriticalDegradationStep * 100.0))
                          << std::endl;
                std::cout << "\nDegradation bucket crossed in round " << (round + 1)
                          << ": "
                          << describe_bucket_crossing(round_parameter_state, after_round_parameter_state)
                          << ". Keeping the completed electrical and reliability results; "
                             "rerunning thermal only with the updated parameters."
                          << std::endl;

                thermal_action = "rerun";
                std::cout << "TRACEPV_THERMAL action=rerun"
                          << " iteration=" << mission_profile_iteration
                          << " round=" << (round + 1) << "/" << options.num_rounds
                          << " pass=" << round_attempt
                          << " critical_step_percent="
                          << static_cast<int>(std::lround(kCriticalDegradationStep * 100.0))
                          << std::endl;

                const double updated_capacitor_thermal_resistance_scale =
                    1.0 + static_cast<double>(after_round_parameter_state[4]) *
                              kCriticalDegradationStep;
                const double updated_igbt_thermal_resistance_scale =
                    1.0 + static_cast<double>(std::max(after_round_parameter_state[5],
                                                       after_round_parameter_state[6])) *
                              kCriticalDegradationStep;

                CapacitorCoefficients updated_cap_coeffs;
                CapacitorType updated_cap_type;
                std::string updated_cap_voltage_type;
                bool updated_cap_coeffs_loaded = false;
                if (!sim_model.capacitor_part_number.empty()) {
                    updated_cap_coeffs_loaded = component_db.load_capacitor(
                        sim_model.capacitor_part_number,
                        updated_cap_coeffs,
                        updated_cap_type,
                        updated_cap_voltage_type);
                }
                const double updated_rth_amb =
                    (updated_cap_coeffs_loaded ? updated_cap_coeffs.rth_amb : 0.5) *
                    updated_capacitor_thermal_resistance_scale;
                const double updated_rth_surf =
                    (updated_cap_coeffs_loaded ? updated_cap_coeffs.rth_surf : 0.3) *
                    updated_capacitor_thermal_resistance_scale;
                const double igbt_scale_ratio =
                    updated_igbt_thermal_resistance_scale /
                    std::max(1e-12, igbt_thermal_resistance_scale);
                const int round_start_case = case_index - round_cases;
                for (int i = round_start_case; i < case_index; ++i) {
                    const std::size_t idx = static_cast<std::size_t>(i);
                    const CapacitorThermalResult updated_capacitor_thermal =
                        calculate_capacitor_thermal(
                            validation_capacitor_loss[idx],
                            validation_internal_temperature[idx],
                            updated_rth_amb,
                            updated_rth_surf);
                    validation_capacitor_hotspot[idx] =
                        updated_capacitor_thermal.capacitor_hotspot_temperature;
                    validation_capacitor_surface[idx] =
                        updated_capacitor_thermal.capacitor_surface_temperature;

                    const double ambient = all_cases[idx].ambient_temperature;
                    validation_igbt_junction[idx] =
                        ambient + (validation_igbt_junction[idx] - ambient) * igbt_scale_ratio;
                }

                std::cout << "TRACEPV_THERMAL_COMPLETE"
                          << " iteration=" << mission_profile_iteration
                          << " round=" << (round + 1) << "/" << options.num_rounds
                          << " capacitor_rth_scale="
                          << updated_capacitor_thermal_resistance_scale
                          << " igbt_rth_scale=" << updated_igbt_thermal_resistance_scale
                          << " electrical_reused=true reliability_results_preserved=true"
                          << std::endl;
            }

            // Write the accepted thermal state after any thermal-only rerun so
            // validation exports always contain the latest temperatures.
            const std::string thermal_csv_filename =
                "results/capacitor_thermal_round_" + std::to_string(current_round + 1) +
                "_iteration_" + std::to_string(mission_profile_iteration) + ".csv";
            std::ofstream thermal_csv(thermal_csv_filename);
            if (thermal_csv.is_open()) {
                thermal_csv
                    << "case_index,time,ambient_temperature,internal_temperature,ac_power,"
                       "capacitor_loss,capacitor_surface_temperature,"
                       "capacitor_hotspot_temperature,igbt_junction_temperature,"
                       "reference_model_used\n";
                thermal_csv << std::setprecision(15);
                const int round_start_case = case_index - round_cases;
                for (int i = round_start_case; i < case_index; ++i) {
                    const std::size_t idx = static_cast<std::size_t>(i);
                    thermal_csv << i << "," << all_cases[idx].time << ","
                                << all_cases[idx].ambient_temperature << ","
                                << validation_internal_temperature[idx] << ","
                                << validation_ac_power[idx] << ","
                                << validation_capacitor_loss[idx] << ","
                                << validation_capacitor_surface[idx] << ","
                                << validation_capacitor_hotspot[idx] << ","
                                << validation_igbt_junction[idx] << ","
                                << validation_reference_model_used[idx] << "\n";
                }
                std::cout << "  Thermal validation data saved to: "
                          << thermal_csv_filename << std::endl;
            } else {
                std::cerr << "  Warning: Failed to write thermal CSV: "
                          << thermal_csv_filename << std::endl;
            }
            
            // Early termination: break out of round loop if any cumulative iteration stressor >= 1.0
            // Check both round-level (for immediate termination) and iteration-level (cumulative across rounds)
            bool should_terminate = round_terminated_early;
            if (!should_terminate) {
                // Check cumulative iteration stressors (including all previous rounds in this iteration)
                if (iteration_fan_stressor_electrical_external >= 1.0 ||
                    iteration_fan_stressor_electrical_internal >= 1.0 ||
                    iteration_fan_stressor_mechanical_external >= 1.0 ||
                    iteration_fan_stressor_mechanical_internal >= 1.0 ||
                    iteration_capacitor_stressor >= 1.0 ||
                    iteration_igbt_stressor_deltaT >= 1.0 ||
                    iteration_igbt_stressor_arrhenius >= 1.0 ||
                    iteration_pcb_degradation >= 1.0) {
                    should_terminate = true;
                    std::cout << "\nEarly termination: Cumulative iteration stressor >= 1.0 detected after round " 
                              << (round + 1) << std::endl;
                }
            }
            
            // Check if any component has exceeded threshold during this iteration
            // (before accumulating to global, check if current iteration + previous global >= 1.0)
            if ((global_igbt_stressor_deltaT + iteration_igbt_stressor_deltaT) >= 1.0) {
                std::cerr << "DEBUG: IGBT DeltaT stressor will exceed 1.0 after this iteration!" << std::endl;
                std::cerr << "  Current global: " << global_igbt_stressor_deltaT << std::endl;
                std::cerr << "  This iteration: " << iteration_igbt_stressor_deltaT << std::endl;
                std::cerr << "  Total will be: " << (global_igbt_stressor_deltaT + iteration_igbt_stressor_deltaT) << std::endl;
            }
            
            const auto round_end = std::chrono::steady_clock::now();
            const double round_time = std::chrono::duration<double>(round_end - round_start).count();
            
            std::cout << "Round " << (current_round + 1) << "/" << options.num_rounds 
                      << " finished: " << cases_processed_in_round << " cases in " 
                      << std::fixed << std::setprecision(2) << round_time << " s" << std::endl;
            
            // Print accumulated stressors for this round
            std::cout << "\nAccumulated Degradation Progress (Round " << (current_round + 1) << "):" << std::endl;
            std::cout << "  Cooling Fan:" << std::endl;
            std::cout << "    Electrical (External): " << std::scientific << std::setprecision(6) 
                      << round_fan_stressor_electrical_external << std::endl;
            std::cout << "    Electrical (Internal): " << std::scientific << std::setprecision(6) 
                      << round_fan_stressor_electrical_internal << std::endl;
            std::cout << "    Mechanical (External): " << std::scientific << std::setprecision(6) 
                      << round_fan_stressor_mechanical_external << std::endl;
            std::cout << "    Mechanical (Internal): " << std::scientific << std::setprecision(6) 
                      << round_fan_stressor_mechanical_internal << std::endl;
            std::cout << "  Capacitor: " << std::scientific << std::setprecision(6) 
                      << round_capacitor_stressor << std::endl;
            std::cout << "  IGBT (DeltaT model): " << std::scientific << std::setprecision(6) 
                      << round_igbt_stressor_deltaT << std::endl;
            std::cout << "  IGBT (Arrhenius model): " << std::scientific << std::setprecision(6) 
                      << round_igbt_stressor_arrhenius << std::endl;
            std::cout << "  PCB: " << std::scientific << std::setprecision(6) 
                      << round_pcb_degradation << std::endl;
            
            // Print cumulative totals for this iteration
            std::cout << "\nCumulative Degradation Progress (This Iteration):" << std::endl;
            std::cout << "  Cooling Fan:" << std::endl;
            std::cout << "    Electrical (External): " << std::scientific << std::setprecision(6) 
                      << iteration_fan_stressor_electrical_external << std::endl;
            std::cout << "    Electrical (Internal): " << std::scientific << std::setprecision(6) 
                      << iteration_fan_stressor_electrical_internal << std::endl;
            std::cout << "    Mechanical (External): " << std::scientific << std::setprecision(6) 
                      << iteration_fan_stressor_mechanical_external << std::endl;
            std::cout << "    Mechanical (Internal): " << std::scientific << std::setprecision(6) 
                      << iteration_fan_stressor_mechanical_internal << std::endl;
            std::cout << "  Capacitor: " << std::scientific << std::setprecision(6) 
                      << iteration_capacitor_stressor << std::endl;
            std::cout << "  IGBT (DeltaT model): " << std::scientific << std::setprecision(6) 
                      << iteration_igbt_stressor_deltaT << std::endl;
            std::cout << "  IGBT (Arrhenius model): " << std::scientific << std::setprecision(6) 
                      << iteration_igbt_stressor_arrhenius << std::endl;
            std::cout << "  PCB: " << std::scientific << std::setprecision(6) 
                      << iteration_pcb_degradation << std::endl;
            // Atomic machine-readable snapshot. The WebUI only publishes
            // degradation and thermal state from accepted round snapshots so
            // all displayed values advance together after a round completes.
            std::cout << "TRACEPV_ROUND_COMPLETE"
                      << " iteration=" << mission_profile_iteration
                      << " round=" << (current_round + 1) << "/" << options.num_rounds
                      << " pass=" << round_attempt
                      << " thermal_action=" << thermal_action
                      << std::scientific << std::setprecision(15)
                      << " fan_electrical_external="
                      << (global_fan_stressor_electrical_external + iteration_fan_stressor_electrical_external)
                      << " fan_electrical_internal="
                      << (global_fan_stressor_electrical_internal + iteration_fan_stressor_electrical_internal)
                      << " fan_mechanical_external="
                      << (global_fan_stressor_mechanical_external + iteration_fan_stressor_mechanical_external)
                      << " fan_mechanical_internal="
                      << (global_fan_stressor_mechanical_internal + iteration_fan_stressor_mechanical_internal)
                      << " capacitor="
                      << (global_capacitor_stressor + iteration_capacitor_stressor)
                      << " igbt_delta_t="
                      << (global_igbt_stressor_deltaT + iteration_igbt_stressor_deltaT)
                      << " igbt_arrhenius="
                      << (global_igbt_stressor_arrhenius + iteration_igbt_stressor_arrhenius)
                      << " pcb="
                      << (global_pcb_degradation + iteration_pcb_degradation)
                      << std::endl;
            std::cout << std::endl;

            // Publish the accepted terminal batch before leaving the loop so
            // the GUI never loses the final round's degradation snapshot.
            if (should_terminate) {
                break;
            }
            }
            
            // Accumulate iteration stressors to global totals (after all rounds complete)
            global_fan_stressor_electrical_external += iteration_fan_stressor_electrical_external;
            global_fan_stressor_electrical_internal += iteration_fan_stressor_electrical_internal;
            global_fan_stressor_mechanical_external += iteration_fan_stressor_mechanical_external;
            global_fan_stressor_mechanical_internal += iteration_fan_stressor_mechanical_internal;
            global_capacitor_stressor += iteration_capacitor_stressor;
            global_igbt_stressor_deltaT += iteration_igbt_stressor_deltaT;
            global_igbt_stressor_arrhenius += iteration_igbt_stressor_arrhenius;
            global_pcb_degradation += iteration_pcb_degradation;
            
            // Accumulate iteration timing to global totals
            total_simulation_time += iteration_simulation_time;
            for (int i = 0; i < num_gpus; ++i) {
                gpu_simulation_times[i] += iteration_gpu_simulation_times[i];
            }
            
            // Mark electrical results as stored after first iteration completes
            if (cache_electrical_results && mission_profile_iteration == 1 && !skip_electrical_simulation) {
                electrical_results_stored = true;
                std::cout << "Electrical simulation results stored for reuse in all subsequent iterations." << std::endl;
            } else if (!cache_electrical_results && mission_profile_iteration == 1 && !skip_electrical_simulation) {
                std::cout << "Electrical simulation result cache disabled for single-iteration run." << std::endl;
            }
            
            const auto iteration_end = std::chrono::steady_clock::now();
            const double iteration_duration = std::chrono::duration<double>(iteration_end - iteration_start).count();
            
            std::cout << "\n" << std::string(60, '-') << std::endl;
            std::cout << "Mission Profile Iteration #" << mission_profile_iteration << " Complete" << std::endl;
            std::cout << "  Duration: " << std::fixed << std::setprecision(2) << iteration_duration << " s" << std::endl;
            std::cout << std::string(60, '-') << std::endl;
            
            // Print global cumulative totals (across all iterations)
            std::cout << "\nGlobal Cumulative Degradation Progress (All Iterations):" << std::endl;
            std::cout << "  Cooling Fan:" << std::endl;
            std::cout << "    Electrical (External): " << std::scientific << std::setprecision(6) 
                      << global_fan_stressor_electrical_external;
            if (global_fan_stressor_electrical_external >= 1.0) {
                std::cout << " [FAILED - Reached 1.0]";
            }
            std::cout << std::endl;
            std::cout << "    Electrical (Internal): " << std::scientific << std::setprecision(6) 
                      << global_fan_stressor_electrical_internal;
            if (global_fan_stressor_electrical_internal >= 1.0) {
                std::cout << " [FAILED - Reached 1.0]";
            }
            std::cout << std::endl;
            std::cout << "    Mechanical (External): " << std::scientific << std::setprecision(6) 
                      << global_fan_stressor_mechanical_external;
            if (global_fan_stressor_mechanical_external >= 1.0) {
                std::cout << " [FAILED - Reached 1.0]";
            }
            std::cout << std::endl;
            std::cout << "    Mechanical (Internal): " << std::scientific << std::setprecision(6) 
                      << global_fan_stressor_mechanical_internal;
            if (global_fan_stressor_mechanical_internal >= 1.0) {
                std::cout << " [FAILED - Reached 1.0]";
            }
            std::cout << std::endl;
            std::cout << "  Capacitor: " << std::scientific << std::setprecision(6) 
                      << global_capacitor_stressor;
            if (global_capacitor_stressor >= 1.0) {
                std::cout << " [FAILED - Reached 1.0]";
            }
            std::cout << std::endl;
            std::cout << "  IGBT (DeltaT model): " << std::scientific << std::setprecision(6) 
                      << global_igbt_stressor_deltaT;
            if (global_igbt_stressor_deltaT >= 1.0) {
                std::cout << " [FAILED - Reached 1.0]";
            }
            std::cout << std::endl;
            std::cout << "  IGBT (Arrhenius model): " << std::scientific << std::setprecision(6) 
                      << global_igbt_stressor_arrhenius;
            if (global_igbt_stressor_arrhenius >= 1.0) {
                std::cout << " [FAILED - Reached 1.0]";
            }
            std::cout << std::endl;
            std::cout << "  PCB: " << std::scientific << std::setprecision(6) 
                      << global_pcb_degradation;
            if (global_pcb_degradation >= 1.0) {
                std::cout << " [FAILED - Reached 1.0]";
            }
            std::cout << std::endl;
            std::cout << std::endl;
            
            // Check if any component has reached degradation = 1.0
            std::cerr << "DEBUG: Checking degradation thresholds:" << std::endl;
            std::cerr << "  global_igbt_stressor_deltaT: " << global_igbt_stressor_deltaT << std::endl;
            std::cerr << "  global_igbt_stressor_arrhenius: " << global_igbt_stressor_arrhenius << std::endl;
            
            if (global_fan_stressor_electrical_external >= 1.0) {
                degradation_reached = true;
                failed_component = "Cooling Fan (Electrical External)";
                std::cerr << "DEBUG: Degradation reached - Cooling Fan (Electrical External)" << std::endl;
            } else if (global_fan_stressor_electrical_internal >= 1.0) {
                degradation_reached = true;
                failed_component = "Cooling Fan (Electrical Internal)";
                std::cerr << "DEBUG: Degradation reached - Cooling Fan (Electrical Internal)" << std::endl;
            } else if (global_fan_stressor_mechanical_external >= 1.0) {
                degradation_reached = true;
                failed_component = "Cooling Fan (Mechanical External)";
                std::cerr << "DEBUG: Degradation reached - Cooling Fan (Mechanical External)" << std::endl;
            } else if (global_fan_stressor_mechanical_internal >= 1.0) {
                degradation_reached = true;
                failed_component = "Cooling Fan (Mechanical Internal)";
                std::cerr << "DEBUG: Degradation reached - Cooling Fan (Mechanical Internal)" << std::endl;
            } else if (global_capacitor_stressor >= 1.0) {
                degradation_reached = true;
                failed_component = "Capacitor";
                std::cerr << "DEBUG: Degradation reached - Capacitor" << std::endl;
            } else if (global_igbt_stressor_deltaT >= 1.0) {
                degradation_reached = true;
                failed_component = "IGBT (DeltaT model)";
                std::cerr << "DEBUG: Degradation reached - IGBT (DeltaT model): " << global_igbt_stressor_deltaT << std::endl;
            } else if (global_igbt_stressor_arrhenius >= 1.0) {
                degradation_reached = true;
                failed_component = "IGBT (Arrhenius model)";
                std::cerr << "DEBUG: Degradation reached - IGBT (Arrhenius model): " << global_igbt_stressor_arrhenius << std::endl;
            } else if (global_pcb_degradation >= 1.0) {
                degradation_reached = true;
                failed_component = "PCB";
                std::cerr << "DEBUG: Degradation reached - PCB" << std::endl;
            } else {
                std::cerr << "DEBUG: No degradation threshold reached yet" << std::endl;
            }
            
            const bool reached_iteration_limit =
                !degradation_reached && options.max_iterations > 0 &&
                mission_profile_iteration >= options.max_iterations;

            if (!degradation_reached && !reached_iteration_limit) {
                std::cout << "No component has reached degradation = 1.0 yet." << std::endl;
                std::cout << "Starting next mission profile iteration...\n" << std::endl;
            } else if (reached_iteration_limit) {
                std::cout << "No component has reached degradation = 1.0 yet." << std::endl;
                std::cout << "Max mission profile iterations reached; stopping simulation." << std::endl;
            } else {
                std::cout << "\n" << std::string(60, '=') << std::endl;
                std::cout << "FAILURE DETECTED: " << failed_component << " reached degradation = 1.0" << std::endl;
                std::cout << std::string(60, '=') << std::endl;
            }
        }
        
        // ====================================================================
        // End of top-level while loop
        // ====================================================================

        const bool stopped_by_iteration_limit =
            !degradation_reached && options.max_iterations > 0 &&
            mission_profile_iteration >= options.max_iterations;
        
        std::cout << "\n" << std::string(60, '=') << std::endl;
        if (stopped_by_iteration_limit) {
            std::cout << "SIMULATION STOPPED: MAX ITERATIONS REACHED" << std::endl;
        } else {
            std::cout << "DEGRADATION LIMIT REACHED" << std::endl;
        }
        std::cout << std::string(60, '=') << std::endl;
        std::cout << "Failed Component: "
                  << (stopped_by_iteration_limit ? "None (max iterations reached)" : failed_component)
                  << std::endl;
        std::cout << "Total Mission Profile Iterations: " << mission_profile_iteration << std::endl;
        std::cout << "Total Rounds: " << (mission_profile_iteration * options.num_rounds) << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        
        const auto total_end = std::chrono::steady_clock::now();
        const double total_duration = std::chrono::duration<double>(total_end - total_start).count();
        const auto print_profile_line = [&](const std::string& label, double seconds) {
            const double pct = total_duration > 0.0 ? (100.0 * seconds / total_duration) : 0.0;
            std::cout << "  " << std::left << std::setw(30) << label << std::right
                      << std::fixed << std::setprecision(3) << seconds << " s"
                      << " (" << std::setprecision(1) << pct << "%)" << std::endl;
        };
        const double profiled_time =
            profiling_totals.static_cache_electrical +
            profiling_totals.electrical_gpu +
            profiling_totals.stress_loss_thermal +
            profiling_totals.fan_reliability +
            profiling_totals.capacitor_reliability +
            profiling_totals.igbt_reliability +
            profiling_totals.pcb_reliability +
            profiling_totals.stressor_csv;

        std::cout << "\nProfiling Breakdown:" << std::endl;
        print_profile_line("Static electrical cache", profiling_totals.static_cache_electrical);
        print_profile_line("Electrical GPU batches", profiling_totals.electrical_gpu);
        print_profile_line("Stress/Loss/Thermal", profiling_totals.stress_loss_thermal);
        std::cout << "  Stress/Loss/Thermal detail (included above):" << std::endl;
        print_profile_line("  Stress calculation", profiling_totals.stress_calculation);
        print_profile_line("  Capacitor loss", profiling_totals.capacitor_loss);
        print_profile_line("  Power module loss", profiling_totals.power_module_loss);
        print_profile_line("  Capacitor thermal", profiling_totals.capacitor_thermal);
        std::cout << "    Capacitor reference detail (included in Capacitor thermal):" << std::endl;
        print_profile_line("    Harmonic extraction", profiling_totals.capacitor_ref_harmonic);
        print_profile_line("    ESR grid", profiling_totals.capacitor_ref_esr_grid);
        print_profile_line("    Loss grid", profiling_totals.capacitor_ref_loss_grid);
        print_profile_line("    Polynomial fit", profiling_totals.capacitor_ref_polyfit);
        print_profile_line("    Thermal iteration", profiling_totals.capacitor_ref_iteration);
        print_profile_line("    Fallback static thermal", profiling_totals.capacitor_fallback_static);
        print_profile_line("  IGBT thermal", profiling_totals.igbt_thermal);
        std::cout << "    IGBT reference detail (included in IGBT thermal):" << std::endl;
        print_profile_line("    Parameter lookup", profiling_totals.igbt_parameter_lookup);
        print_profile_line("    Input/device build", profiling_totals.igbt_input_build);
        print_profile_line("    Loss table IGBT1", profiling_totals.igbt_loss_igbt1);
        print_profile_line("    Loss table IGBT2", profiling_totals.igbt_loss_igbt2);
        print_profile_line("    Loss table Diode1", profiling_totals.igbt_loss_diode1);
        print_profile_line("    Loss table Diode2", profiling_totals.igbt_loss_diode2);
        print_profile_line("    Thermal RC integration", profiling_totals.igbt_thermal_rc);
        print_profile_line("Fan reliability", profiling_totals.fan_reliability);
        print_profile_line("Capacitor reliability", profiling_totals.capacitor_reliability);
        print_profile_line("IGBT reliability", profiling_totals.igbt_reliability);
        print_profile_line("PCB reliability", profiling_totals.pcb_reliability);
        print_profile_line("Stressor CSV output", profiling_totals.stressor_csv);
        print_profile_line("Profiled subtotal", profiled_time);
        
        // Write timing log
        std::string topology_str = std::to_string(topology_level) + "l" + std::to_string(model_stage) + "s";
        std::ofstream timing_log(log_dir / ("batch_timing_" + topology_str + ".log"));
        if (timing_log.is_open()) {
            timing_log << std::fixed << std::setprecision(6);
            timing_log << "Topology: " << topology_str << std::endl;
            timing_log << "Number of GPUs: " << num_gpus << std::endl;
            timing_log << "Precision: " << precision_to_string(options.precision) << std::endl;
            timing_log << "Pipeline: " << (options.pipeline_enabled ? "on" : "off") << std::endl;
            timing_log << "Batch Size Limit: "
                       << (options.batch_size_limit > 0 ? std::to_string(options.batch_size_limit) : std::string("auto"))
                       << std::endl;
            timing_log << "Total Cases per Iteration: " << total_cases << std::endl;
            timing_log << "Batch Size: " << batch_size << std::endl;
            timing_log << "Number of Rounds per Iteration: " << options.num_rounds << std::endl;
            timing_log << "Total Mission Profile Iterations: " << mission_profile_iteration << std::endl;
            timing_log << "Total Rounds: " << (mission_profile_iteration * options.num_rounds) << std::endl;
            timing_log << "Failed Component: "
                       << (stopped_by_iteration_limit ? "None (max iterations reached)" : failed_component)
                       << std::endl;
            timing_log << "Total Wall Clock Time: " << total_duration << " s" << std::endl;
            timing_log << "Simulation Time per GPU:" << std::endl;
            for (int i = 0; i < num_gpus; ++i) {
                timing_log << "  GPU " << i << ": " << gpu_simulation_times[i] << " s" << std::endl;
            }
            timing_log << "Total Compute Time (all GPUs): " << total_simulation_time << " s" << std::endl;
            timing_log << "Average Time per Case: " << (total_duration / (total_cases * mission_profile_iteration)) << " s" << std::endl;
            timing_log << "Speedup: " << (total_simulation_time / total_duration) << "x" << std::endl;
            timing_log << "\nProfiling Breakdown:" << std::endl;
            timing_log << "  Static electrical cache: " << profiling_totals.static_cache_electrical << " s" << std::endl;
            timing_log << "  Electrical GPU batches: " << profiling_totals.electrical_gpu << " s" << std::endl;
            timing_log << "  Stress/Loss/Thermal: " << profiling_totals.stress_loss_thermal << " s" << std::endl;
            timing_log << "  Stress/Loss/Thermal detail included above:" << std::endl;
            timing_log << "    Stress calculation: " << profiling_totals.stress_calculation << " s" << std::endl;
            timing_log << "    Capacitor loss: " << profiling_totals.capacitor_loss << " s" << std::endl;
            timing_log << "    Power module loss: " << profiling_totals.power_module_loss << " s" << std::endl;
            timing_log << "    Capacitor thermal: " << profiling_totals.capacitor_thermal << " s" << std::endl;
            timing_log << "      Capacitor reference detail included in Capacitor thermal:" << std::endl;
            timing_log << "        Harmonic extraction: " << profiling_totals.capacitor_ref_harmonic << " s" << std::endl;
            timing_log << "        ESR grid: " << profiling_totals.capacitor_ref_esr_grid << " s" << std::endl;
            timing_log << "        Loss grid: " << profiling_totals.capacitor_ref_loss_grid << " s" << std::endl;
            timing_log << "        Polynomial fit: " << profiling_totals.capacitor_ref_polyfit << " s" << std::endl;
            timing_log << "        Thermal iteration: " << profiling_totals.capacitor_ref_iteration << " s" << std::endl;
            timing_log << "        Fallback static thermal: " << profiling_totals.capacitor_fallback_static << " s" << std::endl;
            timing_log << "    IGBT thermal: " << profiling_totals.igbt_thermal << " s" << std::endl;
            timing_log << "      IGBT reference detail included in IGBT thermal:" << std::endl;
            timing_log << "        Parameter lookup: " << profiling_totals.igbt_parameter_lookup << " s" << std::endl;
            timing_log << "        Input/device build: " << profiling_totals.igbt_input_build << " s" << std::endl;
            timing_log << "        Loss table IGBT1: " << profiling_totals.igbt_loss_igbt1 << " s" << std::endl;
            timing_log << "        Loss table IGBT2: " << profiling_totals.igbt_loss_igbt2 << " s" << std::endl;
            timing_log << "        Loss table Diode1: " << profiling_totals.igbt_loss_diode1 << " s" << std::endl;
            timing_log << "        Loss table Diode2: " << profiling_totals.igbt_loss_diode2 << " s" << std::endl;
            timing_log << "        Thermal RC integration: " << profiling_totals.igbt_thermal_rc << " s" << std::endl;
            timing_log << "  Fan reliability: " << profiling_totals.fan_reliability << " s" << std::endl;
            timing_log << "  Capacitor reliability: " << profiling_totals.capacitor_reliability << " s" << std::endl;
            timing_log << "  IGBT reliability: " << profiling_totals.igbt_reliability << " s" << std::endl;
            timing_log << "  PCB reliability: " << profiling_totals.pcb_reliability << " s" << std::endl;
            timing_log << "  Stressor CSV output: " << profiling_totals.stressor_csv << " s" << std::endl;
            timing_log << "  Profiled subtotal: " << profiled_time << " s" << std::endl;
            timing_log << "\nFinal Degradation Progress:" << std::endl;
            timing_log << "  Fan Electrical External: " << global_fan_stressor_electrical_external << std::endl;
            timing_log << "  Fan Electrical Internal: " << global_fan_stressor_electrical_internal << std::endl;
            timing_log << "  Fan Mechanical External: " << global_fan_stressor_mechanical_external << std::endl;
            timing_log << "  Fan Mechanical Internal: " << global_fan_stressor_mechanical_internal << std::endl;
            timing_log << "  Capacitor: " << global_capacitor_stressor << std::endl;
            timing_log << "  IGBT (DeltaT model): " << global_igbt_stressor_deltaT << std::endl;
            timing_log << "  IGBT (Arrhenius model): " << global_igbt_stressor_arrhenius << std::endl;
            timing_log << "  PCB: " << global_pcb_degradation << std::endl;
            timing_log.close();
        }
        
        std::cout << "\nSimulation Complete" << std::endl;
        std::cout << "  Total Cases per Iteration: " << total_cases << std::endl;
        std::cout << "  Total Mission Profile Iterations: " << mission_profile_iteration << std::endl;
        std::cout << "  Total Rounds: " << (mission_profile_iteration * options.num_rounds) << std::endl;
        std::cout << "  Execution Time (Wall Clock): " << std::fixed << std::setprecision(2) 
                  << total_duration << " s" << std::endl;
        std::cout << "  Simulation Time per GPU:" << std::endl;
        for (int i = 0; i < num_gpus; ++i) {
            std::cout << "    GPU " << i << ": " << std::fixed << std::setprecision(2) 
                      << gpu_simulation_times[i] << " s" << std::endl;
        }
        std::cout << "  Average Time per Case: " << std::setprecision(4) 
                  << (total_duration / (total_cases * mission_profile_iteration)) << " s" << std::endl;
        std::cout << "  Speedup: " << std::setprecision(2) 
                  << (total_simulation_time / total_duration) << "x" << std::endl;
        
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
