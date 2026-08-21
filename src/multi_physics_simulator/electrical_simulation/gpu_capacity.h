#pragma once

#include <cuda_runtime.h>
#include <string>
#include <vector>

struct GpuCapacity {
    int max_threads_per_block;
    int max_blocks_per_grid;
    int max_threads_per_multiprocessor;
    int multiprocessor_count;
    size_t total_global_memory;  // bytes
    int compute_capability_major;
    int compute_capability_minor;
    std::string device_name;
};

// Query GPU capacity for a specific device
GpuCapacity query_gpu_capacity(int device_id = 0);

// Query all GPUs and return aggregated capacity
struct AggregatedGpuCapacity {
    int num_gpus;
    long long total_threads;  // Sum of all threads across all GPUs
    size_t total_memory;      // Sum of all memory across all GPUs
    int total_multiprocessors; // Sum of all multiprocessors across all GPUs
    std::vector<GpuCapacity> gpu_capacities; // Individual GPU capacities
};

AggregatedGpuCapacity query_all_gpu_capacity();

// Calculate batch size based on GPU capacity and simulation requirements
// fundamental_cycle_size: number of samples per fundamental cycle (fundamental_freq / switching_freq)
int calculate_batch_size(const GpuCapacity& capacity, 
                         int fundamental_cycle_size,
                         int num_states_per_case,
                         size_t bytes_per_state);

