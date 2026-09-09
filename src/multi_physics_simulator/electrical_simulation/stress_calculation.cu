#include "stress_calculation.h"
#include "a2s_gpu.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <cuda_runtime.h>

// Helper function to get boost switch state at a given time
// For stage 2, boost switch is ON when t mod T_sw < boost_duty * T_sw
double get_boost_switch_state(double t, const SimulationParameters& params) {
    if (params.model_stage == 1) {
        return 0.0; // No boost stage for single-stage
    }
    
    const double t_mod = std::fmod(t, params.switching_period);
    const double boost_on_time = params.boost_duty * params.switching_period;
    return (t_mod < boost_on_time) ? 1.0 : 0.0;
}

// CUDA kernel to calculate stress for all samples
__global__ void calculate_stress_kernel(
    const SimulationParameters params,
    const double* states,              // Input: State vectors (num_samples * num_states)
    const double* time_points,          // Input: Time points for each sample
    const int* switching_states,         // Input: Switching states [sa, sb, sc] for each sample
    int num_samples,                     // Input: Number of samples
    double* V_ce,                       // Output: Voltage stress on Phase A Top IGBT
    double* I_c,                        // Output: Current stress on Phase A Top IGBT
    double* I_cap                       // Output: DC Link capacitor ripple current
) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_samples) {
        return;
    }
    
    // Get switching states for this sample
    int sa = switching_states[idx * 3 + 0];
    int sb = switching_states[idx * 3 + 1];
    int sc = switching_states[idx * 3 + 2];
    
    // Get time
    double t = time_points[idx];
    
    // Get state vector for this sample
    const double* sample_states = states + idx * params.num_states;
    
    // Calculate stress using device function
    calculate_stress_device(
        params,
        sample_states,
        t,
        sa, sb, sc,
        V_ce[idx],
        I_c[idx],
        I_cap[idx]
    );
}

// Host function to calculate stress waveforms from a2s simulation outputs
// This can use either CPU or GPU implementation
StressResults calculate_stress(const UnifiedOutputs& outputs, 
                               const SimulationParameters& params) {
    StressResults results;
    const int num_samples = outputs.total_samples;
    
    results.V_ce.resize(num_samples);
    results.I_c.resize(num_samples);
    results.I_cap.resize(num_samples);
    results.I_cap_rms = 0.0;  // Initialize RMS value
    
    // Allocate device memory
    double* d_states;
    double* d_time_points;
    int* d_switching_states;
    double* d_V_ce;
    double* d_I_c;
    double* d_I_cap;
    
    const size_t states_size = num_samples * params.num_states * sizeof(double);
    const size_t time_size = num_samples * sizeof(double);
    const size_t switching_size = num_samples * 3 * sizeof(int);
    const size_t output_size = num_samples * sizeof(double);
    
    cudaMalloc(&d_states, states_size);
    cudaMalloc(&d_time_points, time_size);
    cudaMalloc(&d_switching_states, switching_size);
    cudaMalloc(&d_V_ce, output_size);
    cudaMalloc(&d_I_c, output_size);
    cudaMalloc(&d_I_cap, output_size);
    
    // Copy input data to device
    cudaMemcpy(d_states, outputs.states.data(), states_size, cudaMemcpyHostToDevice);
    cudaMemcpy(d_time_points, outputs.time_points.data(), time_size, cudaMemcpyHostToDevice);
    cudaMemcpy(d_switching_states, outputs.switching_states.data(), switching_size, cudaMemcpyHostToDevice);
    
    // Launch kernel
    const int threads_per_block = 256;
    const int num_blocks = (num_samples + threads_per_block - 1) / threads_per_block;
    
    calculate_stress_kernel<<<num_blocks, threads_per_block>>>(
        params,
        d_states,
        d_time_points,
        d_switching_states,
        num_samples,
        d_V_ce,
        d_I_c,
        d_I_cap
    );
    
    // Copy results back
    cudaMemcpy(results.V_ce.data(), d_V_ce, output_size, cudaMemcpyDeviceToHost);
    cudaMemcpy(results.I_c.data(), d_I_c, output_size, cudaMemcpyDeviceToHost);
    cudaMemcpy(results.I_cap.data(), d_I_cap, output_size, cudaMemcpyDeviceToHost);
    
    // For single-stage, adjust I_cap to account for ideal DC source
    if (params.model_stage == 1) {
        // Calculate mean of I_draw (which is stored in I_cap)
        double I_source_est = 0.0;
        for (int i = 0; i < num_samples; ++i) {
            I_source_est += results.I_cap[i];
        }
        I_source_est /= num_samples;
        
        // I_cap = I_source - I_draw
        for (int i = 0; i < num_samples; ++i) {
            results.I_cap[i] = I_source_est - results.I_cap[i];
        }
    }
    
    // Calculate RMS value of capacitor current: rms_value = sqrt(mean(square(current_data)))
    double sum_squares = 0.0;
    for (int i = 0; i < num_samples; ++i) {
        sum_squares += results.I_cap[i] * results.I_cap[i];
    }
    // The waveform is total DC-link bank current; divide only the final RMS
    // to obtain the per-capacitor stress for the parallel devices.
    results.I_cap_rms =
        std::sqrt(sum_squares / num_samples) /
        kDcLinkCapacitorParallelDeviceCount;
    
    // Cleanup
    cudaFree(d_states);
    cudaFree(d_time_points);
    cudaFree(d_switching_states);
    cudaFree(d_V_ce);
    cudaFree(d_I_c);
    cudaFree(d_I_cap);
    
    return results;
}

// Calculate AC power exported by the inverter from I2 (grid-side inductor
// currents) and Vc (filter capacitor voltages).  The state-space current
// reference is positive from the grid towards the inverter, so exported power
// is the negative of v*i.  Public AC-power values use the convention
// "positive = inverter exports power to the grid".
// State vector structure:
//   For 2-level stage 1: [i_L1a, i_L1b, i_L1c, i_L2a, i_L2b, i_L2c, v_Ca, v_Cb, v_Cc] (9 states)
//   For 2-level stage 2: [i_L1a, i_L1b, i_L1c, i_L2a, i_L2b, i_L2c, v_Ca, v_Cb, v_Cc, i_Lb, v_dc] (11 states)
//   For 3-level stage 1: [i_L1a, i_L1b, i_L1c, i_L2a, i_L2b, i_L2c, v_Ca, v_Cb, v_Cc, v_dc1, v_dc2] (11 states)
//   For 3-level stage 2: [i_L1a, i_L1b, i_L1c, i_L2a, i_L2b, i_L2c, v_Ca, v_Cb, v_Cc, i_Lb, v_dc1, v_dc2] (12 states)
// 
// I2 currents: indices 3, 4, 5 (i_L2a, i_L2b, i_L2c)
// Vc voltages: indices 6, 7, 8 (v_Ca, v_Cb, v_Cc)
ACPowerResults calculate_ac_power(const UnifiedOutputs& outputs,
                                  const SimulationParameters& params) {
    ACPowerResults results;
    const int num_samples = outputs.total_samples;
    
    results.p_AC_instantaneous.resize(num_samples);
    
    // State indices for I2 and Vc (same for all topologies)
    const int idx_i_L2a = 3;
    const int idx_i_L2b = 4;
    const int idx_i_L2c = 5;
    const int idx_v_Ca = 6;
    const int idx_v_Cb = 7;
    const int idx_v_Cc = 8;
    
    // First calculate v*i in the state-space passive sign convention.
    for (int k = 0; k < num_samples; ++k) {
        const double* sample_states = outputs.states.data() + k * params.num_states;
        
        // Extract I2 currents and Vc voltages
        const double i_L2a = sample_states[idx_i_L2a];
        const double i_L2b = sample_states[idx_i_L2b];
        const double i_L2c = sample_states[idx_i_L2c];
        const double v_Ca = sample_states[idx_v_Ca];
        const double v_Cb = sample_states[idx_v_Cb];
        const double v_Cc = sample_states[idx_v_Cc];
        
        // Calculate instantaneous power
        results.p_AC_instantaneous[k] = v_Ca * i_L2a + v_Cb * i_L2b + v_Cc * i_L2c;
    }
    
    // Convert the state-space passive sign convention to exported power.
    for (double& power : results.p_AC_instantaneous) {
        power = -power;
    }

    // Calculate average exported active power: P_out = (1/N) * sum(p_AC[k])
    double sum_power = 0.0;
    for (int k = 0; k < num_samples; ++k) {
        sum_power += results.p_AC_instantaneous[k];
    }
    results.P_AC_average = sum_power / num_samples;
    
    return results;
}

// Convert instantaneous power to a single equivalent value (average power)
// This function is used to get a single AC power value for the thermal model
double calculate_equivalent_ac_power(const std::vector<double>& p_AC_instantaneous) {
    if (p_AC_instantaneous.empty()) {
        return 0.0;
    }
    
    // Calculate average power (equivalent to RMS for active power)
    double sum = 0.0;
    for (size_t i = 0; i < p_AC_instantaneous.size(); ++i) {
        sum += p_AC_instantaneous[i];
    }
    return sum / static_cast<double>(p_AC_instantaneous.size());
}
