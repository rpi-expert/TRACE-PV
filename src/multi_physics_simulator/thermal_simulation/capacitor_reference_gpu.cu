#include "capacitor_reference_gpu.h"

#include <algorithm>
#include <cmath>
#include <cuda_runtime.h>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kThreads = 256;
constexpr int kNearest = 12;

__global__ void harmonic_projection_kernel(const double* time,
                                           const double* current,
                                           int n,
                                           double time0,
                                           double duration,
                                           int max_harmonic,
                                           double* rms_out) {
    extern __shared__ double shared[];
    double* sum_a = shared;
    double* sum_b = shared + blockDim.x;
    const int h = blockIdx.x + 1;
    const int tid = threadIdx.x;
    double a = 0.0;
    double b = 0.0;
    const double freq = static_cast<double>(h) / duration;
    for (int i = tid; i < n; i += blockDim.x) {
        const double phase = 2.0 * kPi * freq * (time[i] - time0);
        const double cur = current[i];
        a += cur * cos(phase);
        b += cur * sin(phase);
    }
    sum_a[tid] = a;
    sum_b[tid] = b;
    __syncthreads();

    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride) {
            sum_a[tid] += sum_a[tid + stride];
            sum_b[tid] += sum_b[tid + stride];
        }
        __syncthreads();
    }

    if (tid == 0 && h <= max_harmonic) {
        const double scale = 2.0 / static_cast<double>(n);
        const double peak = hypot(sum_a[0] * scale, sum_b[0] * scale);
        rms_out[h - 1] = peak / sqrt(2.0);
    }
}

__global__ void esr_grid_kernel(const double* harmonic_freq_hz,
                                int harmonic_count,
                                const double* temps,
                                int temp_count,
                                const double* sample_log_freq_khz,
                                const double* sample_temperature,
                                const double* sample_log_esr,
                                int sample_count,
                                double temp_min,
                                double temp_max,
                                double* esr_grid) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = harmonic_count * temp_count;
    if (idx >= total) {
        return;
    }
    const int h = idx / temp_count;
    const int t = idx % temp_count;
    const double x = log10(fmax(harmonic_freq_hz[h] / 1000.0, 1e-12));
    const double y = fmin(fmax(temps[t], temp_min), temp_max);

    double best_d2[kNearest];
    double best_log[kNearest];
    #pragma unroll
    for (int i = 0; i < kNearest; ++i) {
        best_d2[i] = 1e300;
        best_log[i] = 0.0;
    }

    for (int i = 0; i < sample_count; ++i) {
        const double dx = x - sample_log_freq_khz[i];
        const double dy = (y - sample_temperature[i]) / 60.0;
        const double d2 = dx * dx + dy * dy;
        if (d2 < 1e-24) {
            esr_grid[idx] = pow(10.0, sample_log_esr[i]);
            return;
        }
        int worst = 0;
        double worst_d2 = best_d2[0];
        #pragma unroll
        for (int k = 1; k < kNearest; ++k) {
            if (best_d2[k] > worst_d2) {
                worst_d2 = best_d2[k];
                worst = k;
            }
        }
        if (d2 < worst_d2) {
            best_d2[worst] = d2;
            best_log[worst] = sample_log_esr[i];
        }
    }

    double num = 0.0;
    double den = 0.0;
    #pragma unroll
    for (int k = 0; k < kNearest; ++k) {
        if (best_d2[k] < 1e299) {
            const double w = 1.0 / best_d2[k];
            num += w * best_log[k];
            den += w;
        }
    }
    esr_grid[idx] = den > 0.0 ? pow(10.0, num / den) : 0.0;
}

bool has_cuda_device() {
    if (const char* disabled = std::getenv("TRACEPV_DISABLE_CAP_GPU")) {
        if (disabled[0] != '\0' && disabled[0] != '0') {
            return false;
        }
    }
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

double elapsed_seconds(cudaEvent_t start, cudaEvent_t stop) {
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, start, stop);
    return static_cast<double>(ms) / 1000.0;
}

} // namespace

bool calculate_capacitor_harmonics_gpu(
    const std::vector<double>& time,
    const std::vector<double>& current,
    std::vector<std::pair<double, double>>& harmonics,
    double& elapsed_s) {
    elapsed_s = 0.0;
    harmonics.clear();
    const int n = static_cast<int>(std::min(time.size(), current.size()));
    if (n < 4 || !has_cuda_device()) {
        return false;
    }
    const double duration = time[static_cast<std::size_t>(n - 1)] - time[0];
    if (duration <= 0.0) {
        return false;
    }
    const int max_harmonic = std::min(200, n / 2);

    double time_domain_sum_sq = 0.0;
    for (int i = 0; i < n; ++i) {
        time_domain_sum_sq += current[static_cast<std::size_t>(i)] * current[static_cast<std::size_t>(i)];
    }
    const double time_domain_rms = std::sqrt(time_domain_sum_sq / static_cast<double>(n));

    double* d_time = nullptr;
    double* d_current = nullptr;
    double* d_rms = nullptr;
    cudaEvent_t start{}, stop{};
    if (cudaEventCreate(&start) != cudaSuccess || cudaEventCreate(&stop) != cudaSuccess) {
        return false;
    }

    bool ok = true;
    ok = ok && cudaMalloc(&d_time, static_cast<std::size_t>(n) * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_current, static_cast<std::size_t>(n) * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_rms, static_cast<std::size_t>(max_harmonic) * sizeof(double)) == cudaSuccess;
    if (ok) {
        cudaEventRecord(start);
        ok = ok && cudaMemcpy(d_time, time.data(), static_cast<std::size_t>(n) * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        ok = ok && cudaMemcpy(d_current, current.data(), static_cast<std::size_t>(n) * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        if (ok) {
            harmonic_projection_kernel<<<max_harmonic, kThreads, 2 * kThreads * sizeof(double)>>>(
                d_time, d_current, n, time[0], duration, max_harmonic, d_rms);
            ok = ok && cudaGetLastError() == cudaSuccess;
        }
    }

    std::vector<double> rms(static_cast<std::size_t>(max_harmonic), 0.0);
    if (ok) {
        ok = ok && cudaMemcpy(rms.data(), d_rms, rms.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaEventRecord(stop) == cudaSuccess;
        ok = ok && cudaEventSynchronize(stop) == cudaSuccess;
        if (ok) {
            elapsed_s = elapsed_seconds(start, stop);
        }
    }

    cudaFree(d_time);
    cudaFree(d_current);
    cudaFree(d_rms);
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    if (!ok) {
        return false;
    }

    double harmonic_rms_sq = 0.0;
    const double fundamental = 1.0 / duration;
    for (int h = 1; h <= max_harmonic; ++h) {
        const double value = rms[static_cast<std::size_t>(h - 1)];
        if (value > 1e-9) {
            harmonics.push_back({static_cast<double>(h) * fundamental, value});
            harmonic_rms_sq += value * value;
        }
    }
    const double time_domain_rms_sq = time_domain_rms * time_domain_rms;
    if (harmonic_rms_sq > time_domain_rms_sq * 1.05 && harmonic_rms_sq > 0.0) {
        const double scale = time_domain_rms / std::sqrt(harmonic_rms_sq);
        for (auto& harmonic : harmonics) {
            harmonic.second *= scale;
        }
    }
    return true;
}

bool calculate_capacitor_esr_grid_gpu(
    const std::vector<std::pair<double, double>>& harmonics,
    const std::vector<double>& temps,
    const std::vector<double>& sample_log_freq_khz,
    const std::vector<double>& sample_temperature,
    const std::vector<double>& sample_log_esr,
    double temp_min,
    double temp_max,
    std::vector<std::vector<double>>& esr_grid,
    double& elapsed_s) {
    elapsed_s = 0.0;
    esr_grid.clear();
    const int harmonic_count = static_cast<int>(harmonics.size());
    const int temp_count = static_cast<int>(temps.size());
    const int sample_count = static_cast<int>(sample_log_esr.size());
    if (harmonic_count <= 0 || temp_count <= 0 || sample_count <= 0 || !has_cuda_device()) {
        return false;
    }

    std::vector<double> harmonic_freq_hz(static_cast<std::size_t>(harmonic_count));
    for (int i = 0; i < harmonic_count; ++i) {
        harmonic_freq_hz[static_cast<std::size_t>(i)] = harmonics[static_cast<std::size_t>(i)].first;
    }

    double* d_freq = nullptr;
    double* d_temps = nullptr;
    double* d_sample_log_freq = nullptr;
    double* d_sample_temp = nullptr;
    double* d_sample_log_esr = nullptr;
    double* d_grid = nullptr;
    cudaEvent_t start{}, stop{};
    if (cudaEventCreate(&start) != cudaSuccess || cudaEventCreate(&stop) != cudaSuccess) {
        return false;
    }

    const std::size_t grid_count = static_cast<std::size_t>(harmonic_count) * static_cast<std::size_t>(temp_count);
    bool ok = true;
    ok = ok && cudaMalloc(&d_freq, harmonic_freq_hz.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_temps, temps.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_sample_log_freq, sample_log_freq_khz.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_sample_temp, sample_temperature.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_sample_log_esr, sample_log_esr.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_grid, grid_count * sizeof(double)) == cudaSuccess;
    if (ok) {
        cudaEventRecord(start);
        ok = ok && cudaMemcpy(d_freq, harmonic_freq_hz.data(), harmonic_freq_hz.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        ok = ok && cudaMemcpy(d_temps, temps.data(), temps.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        ok = ok && cudaMemcpy(d_sample_log_freq, sample_log_freq_khz.data(), sample_log_freq_khz.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        ok = ok && cudaMemcpy(d_sample_temp, sample_temperature.data(), sample_temperature.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        ok = ok && cudaMemcpy(d_sample_log_esr, sample_log_esr.data(), sample_log_esr.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        if (ok) {
            const int threads = 256;
            const int blocks = static_cast<int>((grid_count + threads - 1) / threads);
            esr_grid_kernel<<<blocks, threads>>>(d_freq, harmonic_count, d_temps, temp_count,
                                                 d_sample_log_freq, d_sample_temp, d_sample_log_esr,
                                                 sample_count, temp_min, temp_max, d_grid);
            ok = ok && cudaGetLastError() == cudaSuccess;
        }
    }

    std::vector<double> flat_grid(grid_count, 0.0);
    if (ok) {
        ok = ok && cudaMemcpy(flat_grid.data(), d_grid, flat_grid.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaEventRecord(stop) == cudaSuccess;
        ok = ok && cudaEventSynchronize(stop) == cudaSuccess;
        if (ok) {
            elapsed_s = elapsed_seconds(start, stop);
        }
    }

    cudaFree(d_freq);
    cudaFree(d_temps);
    cudaFree(d_sample_log_freq);
    cudaFree(d_sample_temp);
    cudaFree(d_sample_log_esr);
    cudaFree(d_grid);
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    if (!ok) {
        return false;
    }

    esr_grid.assign(static_cast<std::size_t>(harmonic_count), std::vector<double>(temps.size(), 0.0));
    for (int h = 0; h < harmonic_count; ++h) {
        for (int t = 0; t < temp_count; ++t) {
            esr_grid[static_cast<std::size_t>(h)][static_cast<std::size_t>(t)] =
                flat_grid[static_cast<std::size_t>(h * temp_count + t)];
        }
    }
    return true;
}

bool calculate_capacitor_esr_grids_gpu_batch(
    const std::vector<std::vector<std::pair<double, double>>>& batch_harmonics,
    const std::vector<double>& temps,
    const std::vector<double>& sample_log_freq_khz,
    const std::vector<double>& sample_temperature,
    const std::vector<double>& sample_log_esr,
    double temp_min,
    double temp_max,
    std::vector<std::vector<std::vector<double>>>& batch_esr_grids,
    double& elapsed_s) {
    elapsed_s = 0.0;
    batch_esr_grids.clear();
    const int temp_count = static_cast<int>(temps.size());
    const int sample_count = static_cast<int>(sample_log_esr.size());
    if (batch_harmonics.empty() || temp_count <= 0 || sample_count <= 0 || !has_cuda_device()) {
        return false;
    }

    std::vector<int> harmonic_offsets(batch_harmonics.size() + 1, 0);
    std::vector<double> harmonic_freq_hz;
    for (std::size_t case_idx = 0; case_idx < batch_harmonics.size(); ++case_idx) {
        harmonic_offsets[case_idx] = static_cast<int>(harmonic_freq_hz.size());
        for (const auto& harmonic : batch_harmonics[case_idx]) {
            harmonic_freq_hz.push_back(harmonic.first);
        }
    }
    harmonic_offsets[batch_harmonics.size()] = static_cast<int>(harmonic_freq_hz.size());
    const int total_harmonics = static_cast<int>(harmonic_freq_hz.size());
    if (total_harmonics <= 0) {
        return false;
    }

    double* d_freq = nullptr;
    double* d_temps = nullptr;
    double* d_sample_log_freq = nullptr;
    double* d_sample_temp = nullptr;
    double* d_sample_log_esr = nullptr;
    double* d_grid = nullptr;
    cudaEvent_t start{}, stop{};
    if (cudaEventCreate(&start) != cudaSuccess || cudaEventCreate(&stop) != cudaSuccess) {
        return false;
    }

    const std::size_t grid_count = static_cast<std::size_t>(total_harmonics) * static_cast<std::size_t>(temp_count);
    bool ok = true;
    ok = ok && cudaMalloc(&d_freq, harmonic_freq_hz.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_temps, temps.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_sample_log_freq, sample_log_freq_khz.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_sample_temp, sample_temperature.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_sample_log_esr, sample_log_esr.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_grid, grid_count * sizeof(double)) == cudaSuccess;
    if (ok) {
        cudaEventRecord(start);
        ok = ok && cudaMemcpy(d_freq, harmonic_freq_hz.data(), harmonic_freq_hz.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        ok = ok && cudaMemcpy(d_temps, temps.data(), temps.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        ok = ok && cudaMemcpy(d_sample_log_freq, sample_log_freq_khz.data(), sample_log_freq_khz.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        ok = ok && cudaMemcpy(d_sample_temp, sample_temperature.data(), sample_temperature.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        ok = ok && cudaMemcpy(d_sample_log_esr, sample_log_esr.data(), sample_log_esr.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
        if (ok) {
            const int threads = 256;
            const int blocks = static_cast<int>((grid_count + threads - 1) / threads);
            esr_grid_kernel<<<blocks, threads>>>(d_freq, total_harmonics, d_temps, temp_count,
                                                 d_sample_log_freq, d_sample_temp, d_sample_log_esr,
                                                 sample_count, temp_min, temp_max, d_grid);
            ok = ok && cudaGetLastError() == cudaSuccess;
        }
    }

    std::vector<double> flat_grid(grid_count, 0.0);
    if (ok) {
        ok = ok && cudaMemcpy(flat_grid.data(), d_grid, flat_grid.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaEventRecord(stop) == cudaSuccess;
        ok = ok && cudaEventSynchronize(stop) == cudaSuccess;
        if (ok) {
            elapsed_s = elapsed_seconds(start, stop);
        }
    }

    cudaFree(d_freq);
    cudaFree(d_temps);
    cudaFree(d_sample_log_freq);
    cudaFree(d_sample_temp);
    cudaFree(d_sample_log_esr);
    cudaFree(d_grid);
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    if (!ok) {
        return false;
    }

    batch_esr_grids.resize(batch_harmonics.size());
    for (std::size_t case_idx = 0; case_idx < batch_harmonics.size(); ++case_idx) {
        const int begin = harmonic_offsets[case_idx];
        const int end = harmonic_offsets[case_idx + 1];
        const int harmonic_count = end - begin;
        auto& case_grid = batch_esr_grids[case_idx];
        case_grid.assign(static_cast<std::size_t>(harmonic_count), std::vector<double>(temps.size(), 0.0));
        for (int h = 0; h < harmonic_count; ++h) {
            for (int t = 0; t < temp_count; ++t) {
                case_grid[static_cast<std::size_t>(h)][static_cast<std::size_t>(t)] =
                    flat_grid[static_cast<std::size_t>((begin + h) * temp_count + t)];
            }
        }
    }
    return true;
}
