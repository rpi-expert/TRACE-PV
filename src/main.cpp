#include "multi_physics_simulator/electrical_simulation/a2s_gpu.h"
#include "simulation_params.h"
#include "simulation_case.h"
#include "multi_physics_simulator/electrical_simulation/gpu_capacity.h"
#include "multi_physics_simulator/electrical_simulation/stress_calculation.h"
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
#include <map>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <omp.h>
#include <cuda_runtime.h>

namespace {

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
    int static_cases = 1;
    int num_rounds = 1;
    ModulationType modulation = ModulationType::SVM;
    int num_gpus = -1;  // -1 means use all available GPUs
};

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
    std::cout << "  --static-cases: Number of repeated static cases (default: 1)" << std::endl;
    std::cout << "  --rounds: Number of rounds to process (default: 1)" << std::endl;
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
        
        // Set OpenMP to use as many threads as GPUs
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
        
        int batch_size = std::min(batch_size_threads, batch_size_memory);
        
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
        std::cout << "  Final Batch Size: " << batch_size << " cases per batch" << std::endl;
        
        int total_cases = static_cast<int>(all_cases.size());
        int cases_per_round = total_cases / options.num_rounds;
        if (cases_per_round == 0) {
            cases_per_round = total_cases;
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
        std::cout << "  Total Cases: " << total_cases << std::endl;
        std::cout << "  Rounds: " << options.num_rounds << std::endl;
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
        
        int mission_profile_iteration = 0;
        bool degradation_reached = false;
        std::string failed_component = "";
        
        // Storage for electrical simulation results from first iteration
        // Used to skip electrical simulation in subsequent iterations (when iteration <= 10)
        struct StoredElectricalResults {
            double I_cap_rms;
            std::vector<double> V_ce;
            std::vector<double> I_c;
            double ac_power;
        };
        std::vector<StoredElectricalResults> stored_electrical_results(total_cases);
        bool electrical_results_stored = false;
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "Starting Degradation Tracking Simulation" << std::endl;
        std::cout << "Will run mission profile repeatedly until one component reaches degradation = 1.0" << std::endl;
        std::cout << "========================================\n" << std::endl;
        

        int threshold = 10;

        while (!degradation_reached) {
            mission_profile_iteration++;
            std::cout << "\n" << std::string(60, '=') << std::endl;
            std::cout << "Mission Profile Iteration #" << mission_profile_iteration << std::endl;
            std::cout << std::string(60, '=') << std::endl;
            
            // Check if electrical simulation can be skipped
            // Skip if iteration > 1 (we have stored results from first iteration) AND iteration <= 10
            bool skip_electrical_simulation = false;
            if (mission_profile_iteration > 1 && mission_profile_iteration <= threshold && electrical_results_stored) {
                skip_electrical_simulation = true;
                std::cout << "Skipping electrical simulation (iteration " << mission_profile_iteration 
                          << " <= 10, using stored results from first iteration)" << std::endl;
            } else if (mission_profile_iteration > 1 && mission_profile_iteration <= threshold && !electrical_results_stored) {
                std::cout << "Warning: Cannot skip electrical simulation - results not stored yet. Running simulation." << std::endl;
            } else if (mission_profile_iteration > threshold) {
                std::cout << "Running electrical simulation (iteration " << mission_profile_iteration 
                          << " > " << threshold << ", skipping not allowed)" << std::endl;
            }
            
            // Reset case index for this mission profile iteration
            int case_index = 0;
            double iteration_simulation_time = 0.0;
            std::vector<double> iteration_gpu_simulation_times(num_gpus, 0.0); // Track time per GPU for this iteration
            int total_cases_processed = 0;
            
            // Mission profile iteration-level accumulated stressors (reset each iteration)
            double iteration_fan_stressor_electrical_external = 0.0;
            double iteration_fan_stressor_electrical_internal = 0.0;
            double iteration_fan_stressor_mechanical_external = 0.0;
            double iteration_fan_stressor_mechanical_internal = 0.0;
            double iteration_capacitor_stressor = 0.0;
            double iteration_igbt_stressor_deltaT = 0.0;  // IGBT stressor from deltaT model
            double iteration_igbt_stressor_arrhenius = 0.0;  // IGBT stressor from Arrhenius model
            double iteration_pcb_degradation = 0.0;
            
            const auto iteration_start = std::chrono::steady_clock::now();
            
            for (int round = 0; round < options.num_rounds; ++round) {
            const auto round_start = std::chrono::steady_clock::now();
            
            int round_cases = (round == options.num_rounds - 1) ? 
                (total_cases - case_index) : cases_per_round;
            
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
            
            // Structure to track per-case stressors for CSV output
            struct CaseStressorRecord {
                int case_index;
                double fan_electrical_external;
                double fan_electrical_internal;
                double fan_mechanical_external;
                double fan_mechanical_internal;
                double capacitor;
                double igbt_deltaT;
                double igbt_arrhenius;
                double pcb;
            };
            std::vector<CaseStressorRecord> round_stressor_records;
            
            // Use OpenMP to distribute batches across GPUs
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
                        
                        for (int batch = cpu_thread_id; batch < round_batches && !round_terminated_early; batch += num_gpus) {
                            int batch_start = case_index + batch * batch_size;
                            int batch_end = std::min(batch_start + batch_size, case_index + round_cases);
                            int batch_cases = batch_end - batch_start;
                            
                            if (batch_cases <= 0) continue;
                            
                            // Prepare batch of parameters
                            std::vector<SimulationParameters> params_batch;
                            params_batch.reserve(batch_cases);
                            
                            for (int i = 0; i < batch_cases; ++i) {
                                int case_idx = batch_start + i;
                                if (case_idx >= total_cases) break;
                                
                                const SimulationCase& sc = all_cases[case_idx];
                                
                                // Get IV curve data for this case
                                static const IVCurveData default_iv_data;  // Default constructor initializes all to 0/false
                                const IVCurveData& iv_data = (case_idx < static_cast<int>(iv_curve_data.size())) ?
                                    iv_curve_data[case_idx] : default_iv_data;
                                
                                // Update parameters for this case with IV curve data
                                SimulationParameters params = base_params;
                                update_params_for_case(params, sc, iv_data);
                                params_batch.push_back(params);
                            }
                            
                            // Process batch of cases together on this GPU (or skip if using stored results)
                            BatchOutputs batch_outputs;
                            if (!skip_electrical_simulation) {
                                // Run electrical simulation
                                batch_outputs = run_unified_gpu_batch(params_batch);
                            } else {
                                // Skip electrical simulation - create empty batch_outputs structure
                                batch_outputs.outputs.resize(params_batch.size());
                                batch_outputs.elapsed_s = 0.0;
                            }
                            
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
                            
                            // Collect ambient data first for internal condition calculation
                            for (size_t case_idx = 0; case_idx < batch_outputs.outputs.size(); ++case_idx) {
                                int actual_case_idx = batch_start + static_cast<int>(case_idx);
                                const SimulationCase& sc = all_cases[actual_case_idx];
                                batch_ambient_temps.push_back(sc.ambient_temperature);
                                batch_ambient_rhs.push_back(sc.rh);
                                batch_ac_voltages.push_back(sc.ac_voltage);
                            }
                            
                            // Calculate internal temperature and RH for all cases in batch
                            std::vector<double> batch_internal_temps;
                            std::vector<double> batch_internal_rhs;
                            calculate_internal_conditions(
                                batch_ambient_temps,
                                batch_ambient_rhs,
                                batch_ac_voltages,
                                batch_internal_temps,
                                batch_internal_rhs
                            );
                            
                            // Process stress calculation, loss model, and thermal model for each case
                            for (size_t case_idx = 0; case_idx < batch_outputs.outputs.size(); ++case_idx) {
                                const SimulationParameters& params = params_batch[case_idx];
                                
                                // Get the corresponding simulation case for ambient conditions
                                int actual_case_idx = batch_start + static_cast<int>(case_idx);
                                const SimulationCase& sc = all_cases[actual_case_idx];
                                
                                // Calculate stress waveforms (V_ce, I_c, I_cap) - includes RMS calculation
                                // OR use stored results if skipping electrical simulation
                                StressResults stress;
                                double ac_power;
                                
                                if (skip_electrical_simulation) {
                                    // Use stored electrical simulation results
                                    if (actual_case_idx < static_cast<int>(stored_electrical_results.size())) {
                                        const StoredElectricalResults& stored = stored_electrical_results[actual_case_idx];
                                        stress.I_cap_rms = stored.I_cap_rms;
                                        stress.V_ce = stored.V_ce;
                                        stress.I_c = stored.I_c;
                                        // I_cap vector not needed for loss/thermal models, but initialize empty for consistency
                                        stress.I_cap.clear();
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
                                    // Run electrical simulation and calculate stress
                                    const UnifiedOutputs& outputs = batch_outputs.outputs[case_idx];
                                    stress = calculate_stress(outputs, params);
                                    
                                    if (sc.has_ac_power) {
                                        ac_power = sc.ac_power;
                                    } else {
                                        // Calculate AC power from I2 (grid-side inductor currents) and Vc (filter capacitor voltages)
                                        ACPowerResults ac_power_results = calculate_ac_power(outputs, params);
                                        ac_power = calculate_equivalent_ac_power(ac_power_results.p_AC_instantaneous);
                                    }
                                    
                                    // Store results after first iteration, or update results when iteration > 10
                                    if (actual_case_idx < static_cast<int>(stored_electrical_results.size())) {
                                        if (mission_profile_iteration == 1) {
                                            // Store results after first iteration
                                            stored_electrical_results[actual_case_idx].I_cap_rms = stress.I_cap_rms;
                                            stored_electrical_results[actual_case_idx].V_ce = stress.V_ce;
                                            stored_electrical_results[actual_case_idx].I_c = stress.I_c;
                                            stored_electrical_results[actual_case_idx].ac_power = ac_power;
                                        } else if (mission_profile_iteration > 10) {
                                            // Update stored results when iteration > 10 (used for next iteration)
                                            stored_electrical_results[actual_case_idx].I_cap_rms = stress.I_cap_rms;
                                            stored_electrical_results[actual_case_idx].V_ce = stress.V_ce;
                                            stored_electrical_results[actual_case_idx].I_c = stress.I_c;
                                            stored_electrical_results[actual_case_idx].ac_power = ac_power;
                                        }
                                    }
                                }
                                
                                // Calculate losses from stress waveforms
                                // 2.1 Capacitor loss model: input I_cap_rms and ESR, output capacitor loss
                                // For 3-level topology: two capacitors in series, so effective ESR is doubled
                                double esr_value = cap_coeffs_loaded ? cap_coeffs.esr : 0.01; // Use default if not loaded
                                if (params.topology_level == 3) {
                                    esr_value = esr_value * 2.0;  // Two capacitors in series: ESR_total = ESR1 + ESR2
                                }
                                CapacitorLossResult capacitor_loss = calculate_capacitor_loss(stress.I_cap_rms, esr_value);
                                
                                // Debug: Print capacitor loss calculation details
                                static bool loss_debug_printed = false;
                                if (!loss_debug_printed && case_idx == 0) {
                                    std::cerr << "DEBUG: Capacitor loss calculation:" << std::endl;
                                    std::cerr << "  I_cap_rms: " << stress.I_cap_rms << " A" << std::endl;
                                    std::cerr << "  ESR: " << esr_value << " Ohm" << std::endl;
                                    std::cerr << "  Capacitor loss: " << capacitor_loss.capacitor_loss << " W" << std::endl;
                                    loss_debug_printed = true;
                                }
                                
                                // 2.2 Power module loss model: input V_ce and I_c, output power module loss
                                PowerModuleLossResult power_module_loss = calculate_power_module_loss(stress.V_ce, stress.I_c);
                                
                                // Calculate thermal response from losses
                                // 2.3 Capacitor thermal model: input capacitor loss, internal temp, rth_amb, rth_surf
                                //     output capacitor hotspot temp and capacitor surface temp
                                double rth_amb_value = cap_coeffs_loaded ? cap_coeffs.rth_amb : 0.5; // Use default if not loaded
                                double rth_surf_value = cap_coeffs_loaded ? cap_coeffs.rth_surf : 0.3; // Use default if not loaded
                                double internal_temp = batch_internal_temps[case_idx];
                                
                                // Debug: Print thermal model inputs
                                static bool thermal_debug_printed = false;
                                if (!thermal_debug_printed && case_idx == 0) {
                                    std::cerr << "DEBUG: Capacitor thermal model inputs:" << std::endl;
                                    std::cerr << "  Capacitor loss: " << capacitor_loss.capacitor_loss << " W" << std::endl;
                                    std::cerr << "  Internal temp: " << internal_temp << " C" << std::endl;
                                    std::cerr << "  rth_amb: " << rth_amb_value << " K/W" << std::endl;
                                    std::cerr << "  rth_surf: " << rth_surf_value << " K/W" << std::endl;
                                    thermal_debug_printed = true;
                                }
                                
                                CapacitorThermalResult capacitor_thermal = calculate_capacitor_thermal(
                                    capacitor_loss.capacitor_loss,
                                    internal_temp,
                                    rth_amb_value,
                                    rth_surf_value
                                );
                                
                                // Debug: Print thermal model output
                                if (!thermal_debug_printed && case_idx == 0) {
                                    std::cerr << "DEBUG: Capacitor thermal model output:" << std::endl;
                                    std::cerr << "  Hotspot temp: " << capacitor_thermal.capacitor_hotspot_temperature << " C" << std::endl;
                                    std::cerr << "  Surface temp: " << capacitor_thermal.capacitor_surface_temperature << " C" << std::endl;
                                }
                                
                                // 2.4 Power module thermal model: input ac_power (calculated from simulation), ambient temp
                                //     output junction temperature
                                //     Formula: Tj = T_amb + ac_power / 30000 * (100 - 45)
                                PowerModuleThermalResult power_module_thermal = calculate_power_module_thermal(
                                    ac_power,
                                    sc.ambient_temperature
                                );
                                
                                // Debug: Print IGBT thermal model output
                                static bool igbt_thermal_debug_printed = false;
                                if (!igbt_thermal_debug_printed && case_idx == 0) {
                                    std::cerr << "DEBUG: IGBT thermal model output:" << std::endl;
                                    std::cerr << "  AC power (thermal input): " << ac_power << " W" << std::endl;
                                    std::cerr << "  Ambient temp: " << sc.ambient_temperature << " C" << std::endl;
                                    std::cerr << "  Junction temp: " << power_module_thermal.junction_temperature << " C" << std::endl;
                                    igbt_thermal_debug_printed = true;
                                }
                                
                                // Collect data for batch reliability assessment
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
                                
                                // ====================================================================
                            // Section 5: Reliability Analysis - Degradation Tracking (Batch Processing on GPU)
                                // ====================================================================
                                
                            // 5.1 Cooling Fan Reliability: Calculate 4 stressors on GPU
                            // 4 situations: internal electrical, internal mechanical, ambient electrical, ambient mechanical
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
                        }
                        
                        // Perform rainflow counting and reliability analysis for IGBT and PCB
                        // after accumulating temperature data across all batches in this thread
                        
                        // 5.2 IGBT Reliability: Two failure mechanisms
                        // 5.2.1 DeltaT model: Rainflow counting on junction temperatures
                        if (!accumulated_junction_temps.empty() && !sim_model.power_module_part_number.empty()) {
                            PowerModuleCoefficients pm_coeffs;
                            if (component_db.load_power_module(sim_model.power_module_part_number, pm_coeffs)) {
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
                        
                        // 5.4 PCB Reliability: Rainflow counting on internal temperatures
                        if (!accumulated_internal_temps.empty() && !sim_model.pcb_part_number.empty()) {
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
                        
                        // Accumulate results (thread-safe with critical section)
                        #pragma omp critical
                        {
                            iteration_simulation_time += thread_simulation_time;
                            iteration_gpu_simulation_times[cpu_thread_id] += thread_simulation_time;
                            cases_processed_in_round += thread_cases_processed;
                            total_cases_processed += thread_cases_processed;
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
            
            // Accumulate round stressors to iteration totals
            iteration_fan_stressor_electrical_external += round_fan_stressor_electrical_external;
            iteration_fan_stressor_electrical_internal += round_fan_stressor_electrical_internal;
            iteration_fan_stressor_mechanical_external += round_fan_stressor_mechanical_external;
            iteration_fan_stressor_mechanical_internal += round_fan_stressor_mechanical_internal;
            iteration_capacitor_stressor += round_capacitor_stressor;
            iteration_igbt_stressor_deltaT += round_igbt_stressor_deltaT;
            iteration_igbt_stressor_arrhenius += round_igbt_stressor_arrhenius;
            iteration_pcb_degradation += round_pcb_degradation;
            
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
            
            if (should_terminate) {
                break;  // Exit the round loop
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
            std::cout << std::endl;
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
            if (mission_profile_iteration == 1 && !skip_electrical_simulation) {
                electrical_results_stored = true;
                std::cout << "Electrical simulation results stored for reuse in subsequent iterations (when iteration <= 10)" << std::endl;
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
            
            if (!degradation_reached) {
                std::cout << "No component has reached degradation = 1.0 yet." << std::endl;
                std::cout << "Starting next mission profile iteration...\n" << std::endl;
            } else {
                std::cout << "\n" << std::string(60, '=') << std::endl;
                std::cout << "FAILURE DETECTED: " << failed_component << " reached degradation = 1.0" << std::endl;
                std::cout << std::string(60, '=') << std::endl;
            }
        }
        
        // ====================================================================
        // End of top-level while loop
        // ====================================================================
        
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "DEGRADATION LIMIT REACHED" << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        std::cout << "Failed Component: " << failed_component << std::endl;
        std::cout << "Total Mission Profile Iterations: " << mission_profile_iteration << std::endl;
        std::cout << "Total Rounds: " << (mission_profile_iteration * options.num_rounds) << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        
        const auto total_end = std::chrono::steady_clock::now();
        const double total_duration = std::chrono::duration<double>(total_end - total_start).count();
        
        // Write timing log
        std::string topology_str = std::to_string(topology_level) + "l" + std::to_string(model_stage) + "s";
        std::ofstream timing_log(log_dir / ("batch_timing_" + topology_str + ".log"));
        if (timing_log.is_open()) {
            timing_log << std::fixed << std::setprecision(6);
            timing_log << "Topology: " << topology_str << std::endl;
            timing_log << "Number of GPUs: " << num_gpus << std::endl;
            timing_log << "Total Cases per Iteration: " << total_cases << std::endl;
            timing_log << "Batch Size: " << batch_size << std::endl;
            timing_log << "Number of Rounds per Iteration: " << options.num_rounds << std::endl;
            timing_log << "Total Mission Profile Iterations: " << mission_profile_iteration << std::endl;
            timing_log << "Total Rounds: " << (mission_profile_iteration * options.num_rounds) << std::endl;
            timing_log << "Failed Component: " << failed_component << std::endl;
            timing_log << "Total Wall Clock Time: " << total_duration << " s" << std::endl;
            timing_log << "Simulation Time per GPU:" << std::endl;
            for (int i = 0; i < num_gpus; ++i) {
                timing_log << "  GPU " << i << ": " << gpu_simulation_times[i] << " s" << std::endl;
            }
            timing_log << "Total Compute Time (all GPUs): " << total_simulation_time << " s" << std::endl;
            timing_log << "Average Time per Case: " << (total_duration / (total_cases * mission_profile_iteration)) << " s" << std::endl;
            timing_log << "Speedup: " << (total_simulation_time / total_duration) << "x" << std::endl;
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
