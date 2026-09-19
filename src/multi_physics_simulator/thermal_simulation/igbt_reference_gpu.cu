#include "igbt_reference_gpu.h"

#include <algorithm>
#include <array>
#include <cuda_runtime.h>
#include <cstdlib>
#include <cmath>
#include <vector>

namespace {

constexpr int kNumTemps = 26;
constexpr double kRatedVoltage = 400.0;

struct DeviceFlat {
    const double* fundamental_t = nullptr;
    const double* fundamental_y = nullptr;
    const int* fundamental_offsets = nullptr;
    const double* rising_i_t = nullptr;
    const double* rising_i_y = nullptr;
    const int* rising_i_offsets = nullptr;
    const double* falling_v_t = nullptr;
    const double* falling_v_y = nullptr;
    const int* falling_v_offsets = nullptr;
    const double* falling_i_t = nullptr;
    const double* falling_i_y = nullptr;
    const int* falling_i_offsets = nullptr;
    const double* rising_v_t = nullptr;
    const double* rising_v_y = nullptr;
    const int* rising_v_offsets = nullptr;
};

struct GridFlat {
    const double* x = nullptr;
    const double* y = nullptr;
    const double* z = nullptr;
    int x_n = 0;
    int y_n = 0;
};

struct HostFlatTimeValue {
    std::vector<double> t;
    std::vector<double> y;
    std::vector<int> offsets;
};

struct HostFlatDevice {
    HostFlatTimeValue fundamental_i;
    HostFlatTimeValue rising_i;
    HostFlatTimeValue falling_v;
    HostFlatTimeValue falling_i;
    HostFlatTimeValue rising_v;
};

struct DeviceBuffers {
    double* fundamental_t = nullptr;
    double* fundamental_y = nullptr;
    int* fundamental_offsets = nullptr;
    double* rising_i_t = nullptr;
    double* rising_i_y = nullptr;
    int* rising_i_offsets = nullptr;
    double* falling_v_t = nullptr;
    double* falling_v_y = nullptr;
    int* falling_v_offsets = nullptr;
    double* falling_i_t = nullptr;
    double* falling_i_y = nullptr;
    int* falling_i_offsets = nullptr;
    double* rising_v_t = nullptr;
    double* rising_v_y = nullptr;
    int* rising_v_offsets = nullptr;
};

struct GridBuffers {
    double* x = nullptr;
    double* y = nullptr;
    double* z = nullptr;
    int x_n = 0;
    int y_n = 0;
};

bool has_cuda_device() {
    if (const char* disabled = std::getenv("TRACEPV_DISABLE_IGBT_GPU")) {
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

__device__ int lower_bound_device(const double* values, int n, double target) {
    int lo = 0;
    int hi = n;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (values[mid] < target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

__device__ void window_range_device(const double* t, const int* offsets, int case_idx,
                                    double tavg, int window, int& begin, int& end) {
    const int offset = offsets[case_idx];
    const int n = offsets[case_idx + 1] - offset;
    const double start_t = static_cast<double>(window) * tavg - 1e-12;
    const double end_t = static_cast<double>(window + 1) * tavg - 1e-12;
    begin = lower_bound_device(t + offset, n, start_t);
    end = lower_bound_device(t + offset, n, end_t);
}

__device__ double grid_eval_device(GridFlat grid, double xv, double yv) {
    if (grid.x_n < 2 || grid.y_n < 2) {
        return 0.0;
    }
    int x0 = 0;
    int x1 = 1;
    if (xv <= grid.x[0]) {
        x0 = 0;
        x1 = 1;
    } else if (xv >= grid.x[grid.x_n - 1]) {
        x1 = grid.x_n - 1;
        x0 = x1 - 1;
    } else {
        x1 = lower_bound_device(grid.x, grid.x_n, xv);
        x0 = x1 - 1;
    }

    int y0 = 0;
    int y1 = 1;
    if (yv <= grid.y[0]) {
        y0 = 0;
        y1 = 1;
    } else if (yv >= grid.y[grid.y_n - 1]) {
        y1 = grid.y_n - 1;
        y0 = y1 - 1;
    } else {
        y1 = lower_bound_device(grid.y, grid.y_n, yv);
        y0 = y1 - 1;
    }

    const double x_den = grid.x[x1] - grid.x[x0];
    const double y_den = grid.y[y1] - grid.y[y0];
    const double wx = x_den == 0.0 ? 0.0 : (xv - grid.x[x0]) / x_den;
    const double wy = y_den == 0.0 ? 0.0 : (yv - grid.y[y0]) / y_den;
    const double z00 = grid.z[y0 * grid.x_n + x0];
    const double z01 = grid.z[y0 * grid.x_n + x1];
    const double z10 = grid.z[y1 * grid.x_n + x0];
    const double z11 = grid.z[y1 * grid.x_n + x1];
    const double zy0 = (1.0 - wx) * z00 + wx * z01;
    const double zy1 = (1.0 - wx) * z10 + wx * z11;
    return (1.0 - wy) * zy0 + wy * zy1;
}

__device__ double conduction_loss_window(DeviceFlat input, GridFlat model, int case_idx,
                                         double tj, double tavg, int window) {
    int start = 0;
    int end = 0;
    window_range_device(input.fundamental_t, input.fundamental_offsets, case_idx, tavg, window, start, end);
    const int offset = input.fundamental_offsets[case_idx];
    const int count = end - start;
    if (count < 2) {
        return 0.0;
    }
    double e_sum = 0.0;
    for (int i = start; i < end - 1; ++i) {
        const int idx = offset + i;
        const double dt = input.fundamental_t[idx + 1] - input.fundamental_t[idx];
        const double cur = input.fundamental_y[idx];
        e_sum += grid_eval_device(model, cur, tj) * cur * dt;
    }
    const int last = offset + end - 1;
    const double dt_last = input.fundamental_t[last] - input.fundamental_t[last - 1];
    const double cur_last = input.fundamental_y[last];
    e_sum += grid_eval_device(model, cur_last, tj) * cur_last * dt_last;
    return e_sum / tavg;
}

__device__ double switching_loss_window(const double* current_t, const double* current_y, const int* current_offsets,
                                        const double* voltage_t, const double* voltage_y, const int* voltage_offsets,
                                        GridFlat model, int case_idx, double tj, double tavg, int window) {
    int c_start = 0;
    int c_end = 0;
    int v_start = 0;
    int v_end = 0;
    window_range_device(current_t, current_offsets, case_idx, tavg, window, c_start, c_end);
    window_range_device(voltage_t, voltage_offsets, case_idx, tavg, window, v_start, v_end);
    const int n_pair = min(c_end - c_start, v_end - v_start);
    if (n_pair <= 0) {
        return 0.0;
    }
    const int c_offset = current_offsets[case_idx];
    const int v_offset = voltage_offsets[case_idx];
    double sum = 0.0;
    for (int i = 0; i < n_pair; ++i) {
        const double cur = current_y[c_offset + c_start + i];
        const double volt = voltage_y[v_offset + v_start + i];
        sum += grid_eval_device(model, cur, tj) * volt / kRatedVoltage;
    }
    return sum / tavg;
}

__global__ void igbt_loss_table_kernel(DeviceFlat input,
                                       GridFlat conduction_grid,
                                       GridFlat turn_on_grid,
                                       GridFlat turn_off_grid,
                                       int case_count,
                                       int num_windows,
                                       double tavg,
                                       bool has_turn_off,
                                       bool inner,
                                       double rgon,
                                       double rgoff,
                                       double* losses) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = case_count * kNumTemps * num_windows;
    if (idx >= total) {
        return;
    }
    const int temp_window = kNumTemps * num_windows;
    const int case_idx = idx / temp_window;
    const int rem = idx - case_idx * temp_window;
    const int window = rem / kNumTemps;
    const int temp_idx = rem - window * kNumTemps;
    const double tj = 25.0 + 5.0 * static_cast<double>(temp_idx);

    const double pcond = conduction_loss_window(input, conduction_grid, case_idx, tj, tavg, window);
    double psw = 0.0;
    if (has_turn_off) {
        double pon = switching_loss_window(input.rising_i_t, input.rising_i_y, input.rising_i_offsets,
                                           input.falling_v_t, input.falling_v_y, input.falling_v_offsets,
                                           turn_on_grid, case_idx, tj, tavg, window);
        double poff = switching_loss_window(input.falling_i_t, input.falling_i_y, input.falling_i_offsets,
                                            input.rising_v_t, input.rising_v_y, input.rising_v_offsets,
                                            turn_off_grid, case_idx, tj, tavg, window);
        if (inner) {
            const double xgon = rgon / 10.0;
            const double xgoff = rgoff / 10.0;
            poff *= 16.12276 / (xgoff + 13.33362) + 0.07055394 * xgoff - 0.1956174;
            pon *= -24.79985 / (xgon + 3.157488) + 0.2659224 * xgon + 6.653987;
        } else {
            const double xgon = rgon / 6.8;
            const double xgoff = rgoff / 6.8;
            poff *= 116.5799376831 / (xgoff + 54.6872724553717) +
                    0.0356026580548811 * xgoff - 1.1261135920061;
            pon *= -496.825849965403 / (xgon + 19.4140545009492) -
                   0.332711842464142 * xgon + 25.6259606688032;
        }
        psw = pon + poff;
    } else {
        double prr = switching_loss_window(input.falling_i_t, input.falling_i_y, input.falling_i_offsets,
                                           input.rising_v_t, input.rising_v_y, input.rising_v_offsets,
                                           turn_on_grid, case_idx, tj, tavg, window);
        const double x = rgon / 10.0;
        prr *= 1.736545 / (x + 1.499951) - 0.00545 * x + 0.3115;
        psw = prr;
    }

    losses[idx] = pcond + psw;
}

__device__ double loss_lookup_window_device(const double* table,
                                            int num_temps,
                                            int num_windows,
                                            double tj,
                                            int window) {
    window = max(0, min(window, num_windows - 1));
    const double* col = table + window * num_temps;
    if (tj <= 25.0) {
        return col[0];
    }
    if (tj >= 150.0) {
        return col[num_temps - 1];
    }
    const double pos = (tj - 25.0) / 5.0;
    const int i0 = max(0, min(static_cast<int>(floor(pos)), num_temps - 2));
    const int i1 = i0 + 1;
    const double w = (tj - (25.0 + 5.0 * static_cast<double>(i0))) / 5.0;
    return (1.0 - w) * col[i0] + w * col[i1];
}

__global__ void igbt_thermal_kernel(const double* loss1,
                                    const double* loss2,
                                    const double* loss3,
                                    const double* loss4,
                                    const double* ambient,
                                    int case_count,
                                    int num_temps,
                                    int num_windows,
                                    double loss_tavg,
                                    double thermal_dt,
                                    double t_end,
                                    double* out_tj,
                                    double* out_t1,
                                    double* out_t2,
                                    double* out_d1,
                                    double* out_d2,
                                    double* out_case,
                                    double* out_hs,
                                    double* out_avg_loss) {
    const int case_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (case_idx >= case_count) {
        return;
    }

    const double amb = ambient[case_idx];
    const int table_stride = num_temps * num_windows;
    const double* tables[4] = {
        loss1 + case_idx * table_stride,
        loss2 + case_idx * table_stride,
        loss3 + case_idx * table_stride,
        loss4 + case_idx * table_stride
    };

    double ths = amb;
    double tcase = amb;
    double nodes[4][6];
    for (int d = 0; d < 4; ++d) {
        for (int i = 0; i < 6; ++i) {
            nodes[d][i] = amb;
        }
    }

    const double r[4][4] = {
        {0.1347, 0.2501, 0.08578, 0.02939},
        {0.1497, 0.414, 0.2195, 0.1168},
        {0.1561, 0.4349, 0.2333, 0.1257},
        {0.1855, 0.4012, 0.2224, 0.191}
    };
    const double c[4][4] = {
        {0.5811, 0.08053, 0.04136, 0.01172},
        {0.587, 0.05586, 0.01219, 0.002754},
        {0.5643, 0.533, 0.0115, 0.0025},
        {0.5013, 0.07472, 0.0145, 0.001744}
    };

    const double denom_hs = 1.0 + thermal_dt / (0.003 * 2097.0);
    const double ths_keep = 1.0 / denom_hs;
    const double ths_power = (thermal_dt / 2097.0) / denom_hs;
    const double ths_amb = (amb * thermal_dt / (0.003 * 2097.0)) / denom_hs;

    const double denom_case = 1.0 + thermal_dt / (0.1 * 3.0);
    const double case_keep = 1.0 / denom_case;
    const double case_power = (thermal_dt / 3.0) / denom_case;
    const double case_upstream = (thermal_dt / (0.1 * 3.0)) / denom_case;

    double node_keep[4][4];
    double node_power[4][4];
    double node_upstream[4][4];
    for (int d = 0; d < 4; ++d) {
        for (int i = 0; i < 4; ++i) {
            const double rc = r[d][i] * c[d][i];
            const double denom = 1.0 + thermal_dt / rc;
            node_keep[d][i] = 1.0 / denom;
            node_power[d][i] = (thermal_dt / c[d][i]) / denom;
            node_upstream[d][i] = (thermal_dt / rc) / denom;
        }
    }

    const unsigned long long n_total =
        static_cast<unsigned long long>(llround(t_end / thermal_dt)) + 1ULL;
    double total_loss_sum = 0.0;
    for (unsigned long long k = 0; k + 1ULL < n_total; ++k) {
        double tw = fmod(static_cast<double>(k) * thermal_dt, 1.0 / 60.0);
        if (tw < 0.0) {
            tw += 1.0 / 60.0;
        }
        int loss_window = static_cast<int>(floor(tw / loss_tavg));
        loss_window = max(0, min(loss_window, num_windows - 1));
        double p[4] = {
            loss_lookup_window_device(tables[0], num_temps, num_windows, nodes[0][5], loss_window),
            loss_lookup_window_device(tables[1], num_temps, num_windows, nodes[1][5], loss_window),
            loss_lookup_window_device(tables[2], num_temps, num_windows, nodes[2][5], loss_window),
            loss_lookup_window_device(tables[3], num_temps, num_windows, nodes[3][5], loss_window)
        };
        const double pleg = 2.0 * (p[0] + p[1] + p[2] + p[3]);
        const double ptotal = 3.0 * pleg;
        total_loss_sum += ptotal;

        ths = ths_keep * ths + ths_power * ptotal + ths_amb;
        tcase = case_keep * tcase + case_power * pleg + case_upstream * ths;
        for (int d = 0; d < 4; ++d) {
            nodes[d][2] = node_keep[d][0] * nodes[d][2] +
                          node_power[d][0] * p[d] +
                          node_upstream[d][0] * tcase;
            for (int i = 3; i < 6; ++i) {
                const int rc_idx = i - 2;
                nodes[d][i] = node_keep[d][rc_idx] * nodes[d][i] +
                              node_power[d][rc_idx] * p[d] +
                              node_upstream[d][rc_idx] * nodes[d][i - 1];
            }
        }
    }

    const double tj = fmax(fmax(nodes[0][5], nodes[1][5]), fmax(nodes[2][5], nodes[3][5]));
    out_tj[case_idx] = tj;
    out_t1[case_idx] = nodes[0][5];
    out_t2[case_idx] = nodes[1][5];
    out_d1[case_idx] = nodes[2][5];
    out_d2[case_idx] = nodes[3][5];
    out_case[case_idx] = tcase;
    out_hs[case_idx] = ths;
    out_avg_loss[case_idx] =
        total_loss_sum / static_cast<double>(max(1ULL, n_total - 1ULL));
}

void append_flat_time_value(const IgbtGpuTimeValue& tv, HostFlatTimeValue& flat) {
    flat.t.insert(flat.t.end(), tv.t.begin(), tv.t.end());
    flat.y.insert(flat.y.end(), tv.y.begin(), tv.y.end());
}

HostFlatDevice flatten_device(const std::vector<std::array<IgbtGpuDeviceInput, 4>>& batch_devices,
                              int device_idx) {
    HostFlatDevice flat;
    const std::size_t case_count = batch_devices.size();
    flat.fundamental_i.offsets.reserve(case_count + 1);
    flat.rising_i.offsets.reserve(case_count + 1);
    flat.falling_v.offsets.reserve(case_count + 1);
    flat.falling_i.offsets.reserve(case_count + 1);
    flat.rising_v.offsets.reserve(case_count + 1);
    for (const auto& devices : batch_devices) {
        const auto& d = devices[static_cast<std::size_t>(device_idx)];
        flat.fundamental_i.offsets.push_back(static_cast<int>(flat.fundamental_i.t.size()));
        append_flat_time_value(d.fundamental_i, flat.fundamental_i);
        flat.rising_i.offsets.push_back(static_cast<int>(flat.rising_i.t.size()));
        append_flat_time_value(d.rising_i, flat.rising_i);
        flat.falling_v.offsets.push_back(static_cast<int>(flat.falling_v.t.size()));
        append_flat_time_value(d.falling_v, flat.falling_v);
        flat.falling_i.offsets.push_back(static_cast<int>(flat.falling_i.t.size()));
        append_flat_time_value(d.falling_i, flat.falling_i);
        flat.rising_v.offsets.push_back(static_cast<int>(flat.rising_v.t.size()));
        append_flat_time_value(d.rising_v, flat.rising_v);
    }
    flat.fundamental_i.offsets.push_back(static_cast<int>(flat.fundamental_i.t.size()));
    flat.rising_i.offsets.push_back(static_cast<int>(flat.rising_i.t.size()));
    flat.falling_v.offsets.push_back(static_cast<int>(flat.falling_v.t.size()));
    flat.falling_i.offsets.push_back(static_cast<int>(flat.falling_i.t.size()));
    flat.rising_v.offsets.push_back(static_cast<int>(flat.rising_v.t.size()));
    return flat;
}

bool copy_time_value(const HostFlatTimeValue& host, double*& d_t, double*& d_y, int*& d_offsets) {
    bool ok = true;
    ok = ok && cudaMalloc(&d_t, host.t.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_y, host.y.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_offsets, host.offsets.size() * sizeof(int)) == cudaSuccess;
    ok = ok && cudaMemcpy(d_t, host.t.data(), host.t.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
    ok = ok && cudaMemcpy(d_y, host.y.data(), host.y.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
    ok = ok && cudaMemcpy(d_offsets, host.offsets.data(), host.offsets.size() * sizeof(int), cudaMemcpyHostToDevice) == cudaSuccess;
    return ok;
}

bool copy_device(const HostFlatDevice& host, DeviceBuffers& buffers) {
    bool ok = true;
    ok = ok && copy_time_value(host.fundamental_i, buffers.fundamental_t, buffers.fundamental_y, buffers.fundamental_offsets);
    ok = ok && copy_time_value(host.rising_i, buffers.rising_i_t, buffers.rising_i_y, buffers.rising_i_offsets);
    ok = ok && copy_time_value(host.falling_v, buffers.falling_v_t, buffers.falling_v_y, buffers.falling_v_offsets);
    ok = ok && copy_time_value(host.falling_i, buffers.falling_i_t, buffers.falling_i_y, buffers.falling_i_offsets);
    ok = ok && copy_time_value(host.rising_v, buffers.rising_v_t, buffers.rising_v_y, buffers.rising_v_offsets);
    return ok;
}

DeviceFlat as_device_flat(const DeviceBuffers& b) {
    return {b.fundamental_t, b.fundamental_y, b.fundamental_offsets,
            b.rising_i_t, b.rising_i_y, b.rising_i_offsets,
            b.falling_v_t, b.falling_v_y, b.falling_v_offsets,
            b.falling_i_t, b.falling_i_y, b.falling_i_offsets,
            b.rising_v_t, b.rising_v_y, b.rising_v_offsets};
}

void free_device(DeviceBuffers& b) {
    cudaFree(b.fundamental_t);
    cudaFree(b.fundamental_y);
    cudaFree(b.fundamental_offsets);
    cudaFree(b.rising_i_t);
    cudaFree(b.rising_i_y);
    cudaFree(b.rising_i_offsets);
    cudaFree(b.falling_v_t);
    cudaFree(b.falling_v_y);
    cudaFree(b.falling_v_offsets);
    cudaFree(b.falling_i_t);
    cudaFree(b.falling_i_y);
    cudaFree(b.falling_i_offsets);
    cudaFree(b.rising_v_t);
    cudaFree(b.rising_v_y);
    cudaFree(b.rising_v_offsets);
}

bool copy_grid(const IgbtGpuGrid2D& host, GridBuffers& buffers) {
    buffers.x_n = host.x_n;
    buffers.y_n = host.y_n;
    bool ok = host.x_n >= 2 && host.y_n >= 2;
    ok = ok && cudaMalloc(&buffers.x, host.x.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&buffers.y, host.y.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&buffers.z, host.z.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMemcpy(buffers.x, host.x.data(), host.x.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
    ok = ok && cudaMemcpy(buffers.y, host.y.data(), host.y.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
    ok = ok && cudaMemcpy(buffers.z, host.z.data(), host.z.size() * sizeof(double), cudaMemcpyHostToDevice) == cudaSuccess;
    return ok;
}

GridFlat as_grid_flat(const GridBuffers& b) {
    return {b.x, b.y, b.z, b.x_n, b.y_n};
}

void free_grid(GridBuffers& b) {
    cudaFree(b.x);
    cudaFree(b.y);
    cudaFree(b.z);
}

void fill_loss_tables(int device_idx, int case_count, int num_windows,
                      const std::vector<double>& flat_losses,
                      std::vector<std::array<IgbtGpuLossTable, 4>>& batch_loss_tables) {
    std::vector<double> temps(kNumTemps);
    for (int j = 0; j < kNumTemps; ++j) {
        temps[static_cast<std::size_t>(j)] = 25.0 + 5.0 * static_cast<double>(j);
    }
    const int per_case = kNumTemps * num_windows;
    for (int c = 0; c < case_count; ++c) {
        auto& table = batch_loss_tables[static_cast<std::size_t>(c)][static_cast<std::size_t>(device_idx)];
        table.num_temps = kNumTemps;
        table.num_windows = num_windows;
        table.temperatures = temps;
        table.losses.resize(static_cast<std::size_t>(per_case));
        std::copy(flat_losses.begin() + static_cast<std::ptrdiff_t>(c * per_case),
                  flat_losses.begin() + static_cast<std::ptrdiff_t>((c + 1) * per_case),
                  table.losses.begin());
    }
}

} // namespace

bool compute_igbt_loss_tables_gpu_batch(
    const std::vector<std::array<IgbtGpuDeviceInput, 4>>& batch_devices,
    const std::array<IgbtGpuGrid2D, 4>& conduction_grids,
    const std::array<IgbtGpuGrid2D, 4>& turn_on_or_recovery_grids,
    const std::array<IgbtGpuGrid2D, 2>& turn_off_grids,
    double tavg,
    double tsim,
    std::vector<std::array<IgbtGpuLossTable, 4>>& batch_loss_tables,
    std::array<double, 4>& elapsed_s) {
    elapsed_s = {0.0, 0.0, 0.0, 0.0};
    batch_loss_tables.clear();
    if (batch_devices.empty() || tavg <= 0.0 || tsim <= 0.0 || !has_cuda_device()) {
        return false;
    }
    const int case_count = static_cast<int>(batch_devices.size());
    const int num_windows = std::max(1, static_cast<int>(std::floor(tsim / tavg)));
    const std::size_t total_count = static_cast<std::size_t>(case_count) *
                                    static_cast<std::size_t>(num_windows) *
                                    static_cast<std::size_t>(kNumTemps);

    batch_loss_tables.resize(batch_devices.size());
    bool ok = true;
    std::array<GridBuffers, 4> d_cond{};
    std::array<GridBuffers, 4> d_on{};
    std::array<GridBuffers, 2> d_off{};
    for (int d = 0; d < 4; ++d) {
        ok = ok && copy_grid(conduction_grids[static_cast<std::size_t>(d)], d_cond[static_cast<std::size_t>(d)]);
        ok = ok && copy_grid(turn_on_or_recovery_grids[static_cast<std::size_t>(d)], d_on[static_cast<std::size_t>(d)]);
    }
    for (int d = 0; d < 2; ++d) {
        ok = ok && copy_grid(turn_off_grids[static_cast<std::size_t>(d)], d_off[static_cast<std::size_t>(d)]);
    }

    double* d_losses = nullptr;
    ok = ok && cudaMalloc(&d_losses, total_count * sizeof(double)) == cudaSuccess;

    for (int device_idx = 0; ok && device_idx < 4; ++device_idx) {
        HostFlatDevice host_device = flatten_device(batch_devices, device_idx);
        DeviceBuffers d_device{};
        cudaEvent_t start{}, stop{};
        ok = ok && cudaEventCreate(&start) == cudaSuccess;
        ok = ok && cudaEventCreate(&stop) == cudaSuccess;
        ok = ok && copy_device(host_device, d_device);
        if (ok) {
            const bool is_igbt = device_idx < 2;
            const bool inner = device_idx == 1 || device_idx == 3;
            const double rgon = device_idx == 0 ? 6.8 : (device_idx == 1 ? 10.0 : (device_idx == 2 ? 6.6 : 10.0));
            const double rgoff = device_idx == 0 ? 6.8 : 10.0;
            GridFlat off_grid = is_igbt ? as_grid_flat(d_off[static_cast<std::size_t>(device_idx)]) : GridFlat{};
            const int threads = 256;
            const int blocks = static_cast<int>((total_count + threads - 1) / threads);
            cudaEventRecord(start);
            igbt_loss_table_kernel<<<blocks, threads>>>(
                as_device_flat(d_device),
                as_grid_flat(d_cond[static_cast<std::size_t>(device_idx)]),
                as_grid_flat(d_on[static_cast<std::size_t>(device_idx)]),
                off_grid,
                case_count,
                num_windows,
                tavg,
                is_igbt,
                inner,
                rgon,
                rgoff,
                d_losses);
            ok = ok && cudaGetLastError() == cudaSuccess;
        }
        std::vector<double> host_losses(total_count, 0.0);
        if (ok) {
            ok = ok && cudaMemcpy(host_losses.data(), d_losses, total_count * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
            ok = ok && cudaEventRecord(stop) == cudaSuccess;
            ok = ok && cudaEventSynchronize(stop) == cudaSuccess;
            if (ok) {
                elapsed_s[static_cast<std::size_t>(device_idx)] = elapsed_seconds(start, stop);
                fill_loss_tables(device_idx, case_count, num_windows, host_losses, batch_loss_tables);
            }
        }
        free_device(d_device);
        cudaEventDestroy(start);
        cudaEventDestroy(stop);
    }

    cudaFree(d_losses);
    for (auto& g : d_cond) {
        free_grid(g);
    }
    for (auto& g : d_on) {
        free_grid(g);
    }
    for (auto& g : d_off) {
        free_grid(g);
    }

    if (!ok) {
        batch_loss_tables.clear();
        return false;
    }
    return true;
}

bool run_igbt_thermal_gpu_batch(
    const std::vector<std::array<IgbtGpuLossTable, 4>>& batch_loss_tables,
    const std::vector<double>& ambient_temperatures,
    double loss_tavg,
    double thermal_dt,
    double thermal_end_time_s,
    std::vector<IgbtGpuThermalResult>& results,
    double& elapsed_s) {
    elapsed_s = 0.0;
    results.clear();
    const int case_count = static_cast<int>(batch_loss_tables.size());
    if (case_count <= 0 || static_cast<int>(ambient_temperatures.size()) != case_count ||
        loss_tavg <= 0.0 || thermal_dt <= 0.0 || thermal_end_time_s <= 0.0 || !has_cuda_device()) {
        return false;
    }
    const int num_temps = batch_loss_tables[0][0].num_temps;
    const int num_windows = batch_loss_tables[0][0].num_windows;
    if (num_temps <= 0 || num_windows <= 0) {
        return false;
    }

    const std::size_t per_case = static_cast<std::size_t>(num_temps) *
                                 static_cast<std::size_t>(num_windows);
    const std::size_t total_count = static_cast<std::size_t>(case_count) * per_case;
    std::array<std::vector<double>, 4> host_losses;
    for (int d = 0; d < 4; ++d) {
        host_losses[static_cast<std::size_t>(d)].resize(total_count, 0.0);
    }
    for (int c = 0; c < case_count; ++c) {
        for (int d = 0; d < 4; ++d) {
            const auto& table = batch_loss_tables[static_cast<std::size_t>(c)][static_cast<std::size_t>(d)];
            if (table.num_temps != num_temps || table.num_windows != num_windows ||
                table.losses.size() != per_case) {
                return false;
            }
            std::copy(table.losses.begin(), table.losses.end(),
                      host_losses[static_cast<std::size_t>(d)].begin() +
                          static_cast<std::ptrdiff_t>(static_cast<std::size_t>(c) * per_case));
        }
    }

    std::array<double*, 4> d_losses{nullptr, nullptr, nullptr, nullptr};
    double* d_ambient = nullptr;
    double* d_tj = nullptr;
    double* d_t1 = nullptr;
    double* d_t2 = nullptr;
    double* d_d1 = nullptr;
    double* d_d2 = nullptr;
    double* d_case = nullptr;
    double* d_hs = nullptr;
    double* d_avg = nullptr;
    cudaEvent_t start{}, stop{};
    bool ok = cudaEventCreate(&start) == cudaSuccess &&
              cudaEventCreate(&stop) == cudaSuccess;
    for (int d = 0; ok && d < 4; ++d) {
        ok = ok && cudaMalloc(&d_losses[static_cast<std::size_t>(d)], total_count * sizeof(double)) == cudaSuccess;
        ok = ok && cudaMemcpy(d_losses[static_cast<std::size_t>(d)],
                              host_losses[static_cast<std::size_t>(d)].data(),
                              total_count * sizeof(double),
                              cudaMemcpyHostToDevice) == cudaSuccess;
    }
    ok = ok && cudaMalloc(&d_ambient, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_tj, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_t1, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_t2, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_d1, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_d2, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_case, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_hs, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_avg, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMemcpy(d_ambient, ambient_temperatures.data(),
                          ambient_temperatures.size() * sizeof(double),
                          cudaMemcpyHostToDevice) == cudaSuccess;
    if (ok) {
        const int threads = 128;
        const int blocks = (case_count + threads - 1) / threads;
        cudaEventRecord(start);
        igbt_thermal_kernel<<<blocks, threads>>>(
            d_losses[0], d_losses[1], d_losses[2], d_losses[3],
            d_ambient,
            case_count,
            num_temps,
            num_windows,
            loss_tavg,
            thermal_dt,
            thermal_end_time_s,
            d_tj,
            d_t1,
            d_t2,
            d_d1,
            d_d2,
            d_case,
            d_hs,
            d_avg);
        ok = ok && cudaGetLastError() == cudaSuccess;
    }

    std::vector<double> h_tj(static_cast<std::size_t>(case_count));
    std::vector<double> h_t1(static_cast<std::size_t>(case_count));
    std::vector<double> h_t2(static_cast<std::size_t>(case_count));
    std::vector<double> h_d1(static_cast<std::size_t>(case_count));
    std::vector<double> h_d2(static_cast<std::size_t>(case_count));
    std::vector<double> h_case(static_cast<std::size_t>(case_count));
    std::vector<double> h_hs(static_cast<std::size_t>(case_count));
    std::vector<double> h_avg(static_cast<std::size_t>(case_count));
    if (ok) {
        ok = ok && cudaMemcpy(h_tj.data(), d_tj, h_tj.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_t1.data(), d_t1, h_t1.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_t2.data(), d_t2, h_t2.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_d1.data(), d_d1, h_d1.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_d2.data(), d_d2, h_d2.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_case.data(), d_case, h_case.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_hs.data(), d_hs, h_hs.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_avg.data(), d_avg, h_avg.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaEventRecord(stop) == cudaSuccess;
        ok = ok && cudaEventSynchronize(stop) == cudaSuccess;
        if (ok) {
            elapsed_s = elapsed_seconds(start, stop);
        }
    }

    for (double* p : d_losses) {
        cudaFree(p);
    }
    cudaFree(d_ambient);
    cudaFree(d_tj);
    cudaFree(d_t1);
    cudaFree(d_t2);
    cudaFree(d_d1);
    cudaFree(d_d2);
    cudaFree(d_case);
    cudaFree(d_hs);
    cudaFree(d_avg);
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    if (!ok) {
        return false;
    }

    results.resize(static_cast<std::size_t>(case_count));
    for (int c = 0; c < case_count; ++c) {
        auto& out = results[static_cast<std::size_t>(c)];
        out.valid = true;
        out.junction_temperature = h_tj[static_cast<std::size_t>(c)];
        out.igbt1_temperature = h_t1[static_cast<std::size_t>(c)];
        out.igbt2_temperature = h_t2[static_cast<std::size_t>(c)];
        out.diode1_temperature = h_d1[static_cast<std::size_t>(c)];
        out.diode2_temperature = h_d2[static_cast<std::size_t>(c)];
        out.case_temperature = h_case[static_cast<std::size_t>(c)];
        out.heatsink_temperature = h_hs[static_cast<std::size_t>(c)];
        out.average_total_loss = h_avg[static_cast<std::size_t>(c)];
    }
    return true;
}

bool run_igbt_loss_thermal_fused_gpu_batch(
    const std::vector<std::array<IgbtGpuDeviceInput, 4>>& batch_devices,
    const std::array<IgbtGpuGrid2D, 4>& conduction_grids,
    const std::array<IgbtGpuGrid2D, 4>& turn_on_or_recovery_grids,
    const std::array<IgbtGpuGrid2D, 2>& turn_off_grids,
    const std::vector<double>& ambient_temperatures,
    double loss_tavg,
    double loss_tsim,
    double thermal_dt,
    double thermal_end_time_s,
    std::vector<IgbtGpuThermalResult>& results,
    std::array<double, 4>& loss_elapsed_s,
    double& thermal_elapsed_s) {
    results.clear();
    loss_elapsed_s = {0.0, 0.0, 0.0, 0.0};
    thermal_elapsed_s = 0.0;
    if (batch_devices.empty() || ambient_temperatures.size() != batch_devices.size() ||
        loss_tavg <= 0.0 || loss_tsim <= 0.0 || thermal_dt <= 0.0 ||
        thermal_end_time_s <= 0.0 || !has_cuda_device()) {
        return false;
    }

    const int case_count = static_cast<int>(batch_devices.size());
    const int num_windows = std::max(1, static_cast<int>(std::floor(loss_tsim / loss_tavg)));
    const std::size_t per_table_count = static_cast<std::size_t>(case_count) *
                                        static_cast<std::size_t>(num_windows) *
                                        static_cast<std::size_t>(kNumTemps);

    bool ok = true;
    std::array<GridBuffers, 4> d_cond{};
    std::array<GridBuffers, 4> d_on{};
    std::array<GridBuffers, 2> d_off{};
    for (int d = 0; d < 4; ++d) {
        ok = ok && copy_grid(conduction_grids[static_cast<std::size_t>(d)], d_cond[static_cast<std::size_t>(d)]);
        ok = ok && copy_grid(turn_on_or_recovery_grids[static_cast<std::size_t>(d)], d_on[static_cast<std::size_t>(d)]);
    }
    for (int d = 0; d < 2; ++d) {
        ok = ok && copy_grid(turn_off_grids[static_cast<std::size_t>(d)], d_off[static_cast<std::size_t>(d)]);
    }

    std::array<double*, 4> d_losses{nullptr, nullptr, nullptr, nullptr};
    for (int d = 0; ok && d < 4; ++d) {
        ok = ok && cudaMalloc(&d_losses[static_cast<std::size_t>(d)],
                              per_table_count * sizeof(double)) == cudaSuccess;
    }

    for (int device_idx = 0; ok && device_idx < 4; ++device_idx) {
        HostFlatDevice host_device = flatten_device(batch_devices, device_idx);
        DeviceBuffers d_device{};
        cudaEvent_t start{}, stop{};
        ok = ok && cudaEventCreate(&start) == cudaSuccess;
        ok = ok && cudaEventCreate(&stop) == cudaSuccess;
        ok = ok && copy_device(host_device, d_device);
        if (ok) {
            const bool is_igbt = device_idx < 2;
            const bool inner = device_idx == 1 || device_idx == 3;
            const double rgon = device_idx == 0 ? 6.8 : (device_idx == 1 ? 10.0 : (device_idx == 2 ? 6.6 : 10.0));
            const double rgoff = device_idx == 0 ? 6.8 : 10.0;
            GridFlat off_grid = is_igbt ? as_grid_flat(d_off[static_cast<std::size_t>(device_idx)]) : GridFlat{};
            const int threads = 256;
            const int blocks = static_cast<int>((per_table_count + threads - 1) / threads);
            cudaEventRecord(start);
            igbt_loss_table_kernel<<<blocks, threads>>>(
                as_device_flat(d_device),
                as_grid_flat(d_cond[static_cast<std::size_t>(device_idx)]),
                as_grid_flat(d_on[static_cast<std::size_t>(device_idx)]),
                off_grid,
                case_count,
                num_windows,
                loss_tavg,
                is_igbt,
                inner,
                rgon,
                rgoff,
                d_losses[static_cast<std::size_t>(device_idx)]);
            ok = ok && cudaGetLastError() == cudaSuccess;
            ok = ok && cudaEventRecord(stop) == cudaSuccess;
            ok = ok && cudaEventSynchronize(stop) == cudaSuccess;
            if (ok) {
                loss_elapsed_s[static_cast<std::size_t>(device_idx)] = elapsed_seconds(start, stop);
            }
        }
        free_device(d_device);
        cudaEventDestroy(start);
        cudaEventDestroy(stop);
    }

    double* d_ambient = nullptr;
    double* d_tj = nullptr;
    double* d_t1 = nullptr;
    double* d_t2 = nullptr;
    double* d_d1 = nullptr;
    double* d_d2 = nullptr;
    double* d_case = nullptr;
    double* d_hs = nullptr;
    double* d_avg = nullptr;
    cudaEvent_t thermal_start{}, thermal_stop{};
    ok = ok && cudaEventCreate(&thermal_start) == cudaSuccess;
    ok = ok && cudaEventCreate(&thermal_stop) == cudaSuccess;
    ok = ok && cudaMalloc(&d_ambient, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_tj, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_t1, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_t2, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_d1, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_d2, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_case, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_hs, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMalloc(&d_avg, ambient_temperatures.size() * sizeof(double)) == cudaSuccess;
    ok = ok && cudaMemcpy(d_ambient, ambient_temperatures.data(),
                          ambient_temperatures.size() * sizeof(double),
                          cudaMemcpyHostToDevice) == cudaSuccess;

    if (ok) {
        const int threads = 128;
        const int blocks = (case_count + threads - 1) / threads;
        cudaEventRecord(thermal_start);
        igbt_thermal_kernel<<<blocks, threads>>>(
            d_losses[0], d_losses[1], d_losses[2], d_losses[3],
            d_ambient,
            case_count,
            kNumTemps,
            num_windows,
            loss_tavg,
            thermal_dt,
            thermal_end_time_s,
            d_tj,
            d_t1,
            d_t2,
            d_d1,
            d_d2,
            d_case,
            d_hs,
            d_avg);
        ok = ok && cudaGetLastError() == cudaSuccess;
    }

    std::vector<double> h_tj(static_cast<std::size_t>(case_count));
    std::vector<double> h_t1(static_cast<std::size_t>(case_count));
    std::vector<double> h_t2(static_cast<std::size_t>(case_count));
    std::vector<double> h_d1(static_cast<std::size_t>(case_count));
    std::vector<double> h_d2(static_cast<std::size_t>(case_count));
    std::vector<double> h_case(static_cast<std::size_t>(case_count));
    std::vector<double> h_hs(static_cast<std::size_t>(case_count));
    std::vector<double> h_avg(static_cast<std::size_t>(case_count));
    if (ok) {
        ok = ok && cudaMemcpy(h_tj.data(), d_tj, h_tj.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_t1.data(), d_t1, h_t1.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_t2.data(), d_t2, h_t2.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_d1.data(), d_d1, h_d1.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_d2.data(), d_d2, h_d2.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_case.data(), d_case, h_case.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_hs.data(), d_hs, h_hs.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaMemcpy(h_avg.data(), d_avg, h_avg.size() * sizeof(double), cudaMemcpyDeviceToHost) == cudaSuccess;
        ok = ok && cudaEventRecord(thermal_stop) == cudaSuccess;
        ok = ok && cudaEventSynchronize(thermal_stop) == cudaSuccess;
        if (ok) {
            thermal_elapsed_s = elapsed_seconds(thermal_start, thermal_stop);
        }
    }

    for (double* p : d_losses) {
        cudaFree(p);
    }
    for (auto& g : d_cond) {
        free_grid(g);
    }
    for (auto& g : d_on) {
        free_grid(g);
    }
    for (auto& g : d_off) {
        free_grid(g);
    }
    cudaFree(d_ambient);
    cudaFree(d_tj);
    cudaFree(d_t1);
    cudaFree(d_t2);
    cudaFree(d_d1);
    cudaFree(d_d2);
    cudaFree(d_case);
    cudaFree(d_hs);
    cudaFree(d_avg);
    cudaEventDestroy(thermal_start);
    cudaEventDestroy(thermal_stop);
    if (!ok) {
        return false;
    }

    results.resize(static_cast<std::size_t>(case_count));
    for (int c = 0; c < case_count; ++c) {
        auto& out = results[static_cast<std::size_t>(c)];
        out.valid = true;
        out.junction_temperature = h_tj[static_cast<std::size_t>(c)];
        out.igbt1_temperature = h_t1[static_cast<std::size_t>(c)];
        out.igbt2_temperature = h_t2[static_cast<std::size_t>(c)];
        out.diode1_temperature = h_d1[static_cast<std::size_t>(c)];
        out.diode2_temperature = h_d2[static_cast<std::size_t>(c)];
        out.case_temperature = h_case[static_cast<std::size_t>(c)];
        out.heatsink_temperature = h_hs[static_cast<std::size_t>(c)];
        out.average_total_loss = h_avg[static_cast<std::size_t>(c)];
    }
    return true;
}
