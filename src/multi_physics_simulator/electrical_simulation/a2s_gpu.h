#pragma once

#include "simulation_params.h"

#include <vector>

struct UnifiedOutputs {
    int num_states;
    int total_samples;
    std::vector<double> states; // Row-major: sample-major order
    double elapsed_s;  // elapsed time in seconds
    std::vector<double> time_points;
    std::vector<int> switching_states; // Row-major: [sa, sb, sc] for each sample (3 * total_samples)
};

struct BatchOutputs {
    std::vector<UnifiedOutputs> outputs; // One output per case
    double elapsed_s;  // Total elapsed time for the batch
};

UnifiedOutputs run_unified_gpu(const SimulationParameters& params);
BatchOutputs run_unified_gpu_batch(const std::vector<SimulationParameters>& params_batch);

// Device function declarations (for use in other CUDA files)
#ifdef __CUDACC__
// Device function to calculate stress waveforms from a2s simulation outputs
__device__ void calculate_stress_device(
    const SimulationParameters& params,
    const double* states,  // Current state vector
    double t,              // Current time
    int sa, int sb, int sc, // Switching states
    double& V_ce,          // Output: Voltage stress on Phase A Top IGBT (V)
    double& I_c,           // Output: Current stress on Phase A Top IGBT (A)
    double& I_cap          // Output: DC Link capacitor ripple current (A)
);
#endif

