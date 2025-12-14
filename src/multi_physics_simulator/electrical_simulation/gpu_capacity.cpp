#include "gpu_capacity.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <sstream>

GpuCapacity query_gpu_capacity(int device_id) {
    GpuCapacity cap{};
    
    cudaDeviceProp prop;
    cudaError_t err = cudaGetDeviceProperties(&prop, device_id);
    if (err != cudaSuccess) {
        throw std::runtime_error("Failed to get device properties for device " + std::to_string(device_id));
    }
    
    cap.max_threads_per_block = prop.maxThreadsPerBlock;
    cap.max_blocks_per_grid = prop.maxGridSize[0];  // X dimension
    cap.max_threads_per_multiprocessor = prop.maxThreadsPerMultiProcessor;
    cap.multiprocessor_count = prop.multiProcessorCount;
    cap.total_global_memory = prop.totalGlobalMem;
    cap.compute_capability_major = prop.major;
    cap.compute_capability_minor = prop.minor;
    cap.device_name = prop.name;
    
    return cap;
}

AggregatedGpuCapacity query_all_gpu_capacity() {
    AggregatedGpuCapacity agg{};
    
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count == 0) {
        throw std::runtime_error("No CUDA devices found");
    }
    
    agg.num_gpus = device_count;
    agg.total_threads = 0;
    agg.total_memory = 0;
    agg.total_multiprocessors = 0;
    agg.gpu_capacities.reserve(device_count);
    
    // Query each GPU individually
    for (int i = 0; i < device_count; ++i) {
        GpuCapacity cap = query_gpu_capacity(i);
        agg.gpu_capacities.push_back(cap);
        
        // Sum up capacities
        long long threads_per_gpu = static_cast<long long>(cap.multiprocessor_count) * 
                                   static_cast<long long>(cap.max_threads_per_multiprocessor);
        agg.total_threads += threads_per_gpu;
        agg.total_memory += cap.total_global_memory;
        agg.total_multiprocessors += cap.multiprocessor_count;
    }
    
    return agg;
}

int calculate_batch_size(const GpuCapacity& capacity,
                         int fundamental_cycle_size,
                         int num_states_per_case,
                         size_t bytes_per_state) {
    // Calculate memory per simulation case
    // Each case needs:
    // - States: fundamental_cycle_size * num_states_per_case * sizeof(double)
    // - Time points: fundamental_cycle_size * sizeof(double)
    // - Switching states: fundamental_cycle_size * 3 * sizeof(int)
    // - Schedules: depends on number of periods, but estimate conservatively
    // - Additional overhead for intermediate calculations
    
    const size_t bytes_per_sample = num_states_per_case * bytes_per_state + 
                                    sizeof(double) +  // time point
                                    3 * sizeof(int);  // switching states
    
    const size_t bytes_per_case = fundamental_cycle_size * bytes_per_sample;
    
    // Add overhead for schedules and intermediate calculations (estimate 20% overhead)
    const size_t bytes_per_case_with_overhead = static_cast<size_t>(bytes_per_case * 1.2);
    
    // Calculate based on available memory (use 80% of total to leave room for other operations)
    const size_t available_memory = static_cast<size_t>(capacity.total_global_memory * 0.8);
    int batch_size_memory = static_cast<int>(available_memory / bytes_per_case_with_overhead);
    
    // Note: Thread-based batch size calculation is now done in main.cpp
    // because it depends on switching_periods_per_fundamental, not fundamental_cycle_size
    
    // Ensure batch size is at least 1
    batch_size_memory = std::max(1, batch_size_memory);
    
    return batch_size_memory;
}

