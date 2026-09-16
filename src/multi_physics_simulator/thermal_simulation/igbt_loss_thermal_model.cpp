#include "igbt_loss_thermal_model.h"
#include "igbt_reference_gpu.h"
#include "reporting/console_output.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <omp.h>

namespace {

constexpr double kRatedVoltage = 400.0;
constexpr double kDefaultSwitchingFrequency = 16000.0;
constexpr double kLossCycle = 1.0 / 60.0;

struct TimeValue {
    std::vector<double> t;
    std::vector<double> y;
};

struct DeviceInput {
    TimeValue fundamental_i;
    TimeValue rising_i;
    TimeValue falling_v;
    TimeValue falling_i;
    TimeValue rising_v;
};

struct Grid2D {
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> z;
    int x_n = 0;
    int y_n = 0;

    double at(int yi, int xi) const {
        return z[static_cast<std::size_t>(yi * x_n + xi)];
    }

    double eval(double xv, double yv) const {
        if (x_n < 2 || y_n < 2) {
            return 0.0;
        }

        auto span = [](const std::vector<double>& axis, double v) {
            int n = static_cast<int>(axis.size());
            int i0 = 0;
            int i1 = 1;
            if (v <= axis.front()) {
                i0 = 0;
                i1 = 1;
            } else if (v >= axis.back()) {
                i1 = n - 1;
                i0 = i1 - 1;
            } else {
                auto it = std::lower_bound(axis.begin(), axis.end(), v);
                i1 = static_cast<int>(it - axis.begin());
                i0 = i1 - 1;
            }
            double den = axis[static_cast<std::size_t>(i1)] - axis[static_cast<std::size_t>(i0)];
            double w = den == 0.0 ? 0.0 : (v - axis[static_cast<std::size_t>(i0)]) / den;
            return std::array<double, 3>{static_cast<double>(i0), static_cast<double>(i1), w};
        };

        auto xs = span(x, xv);
        auto ys = span(y, yv);
        int x0 = static_cast<int>(xs[0]);
        int x1 = static_cast<int>(xs[1]);
        int y0 = static_cast<int>(ys[0]);
        int y1 = static_cast<int>(ys[1]);
        double wx = xs[2];
        double wy = ys[2];

        double z00 = at(y0, x0);
        double z01 = at(y0, x1);
        double z10 = at(y1, x0);
        double z11 = at(y1, x1);
        double zy0 = (1.0 - wx) * z00 + wx * z01;
        double zy1 = (1.0 - wx) * z10 + wx * z11;
        return (1.0 - wy) * zy0 + wy * zy1;
    }
};

struct LossTable {
    int num_temps = 0;
    int num_windows = 0;
    std::vector<double> temperatures;
    std::vector<double> losses; // column-major: temp + window*num_temps
};

struct ParameterSet {
    Grid2D vce_14, eon_14, eoff_14;
    Grid2D vce_23, eon_23, eoff_23;
    Grid2D vf_14, err_14;
    Grid2D vf_23, err_23;
};

IgbtGpuGrid2D to_gpu_grid(const Grid2D& grid) {
    IgbtGpuGrid2D out;
    out.x = grid.x;
    out.y = grid.y;
    out.z = grid.z;
    out.x_n = grid.x_n;
    out.y_n = grid.y_n;
    return out;
}

IgbtGpuTimeValue to_gpu_time_value(const TimeValue& tv) {
    IgbtGpuTimeValue out;
    out.t = tv.t;
    out.y = tv.y;
    return out;
}

IgbtGpuDeviceInput to_gpu_device_input(const DeviceInput& input) {
    IgbtGpuDeviceInput out;
    out.fundamental_i = to_gpu_time_value(input.fundamental_i);
    out.rising_i = to_gpu_time_value(input.rising_i);
    out.falling_v = to_gpu_time_value(input.falling_v);
    out.falling_i = to_gpu_time_value(input.falling_i);
    out.rising_v = to_gpu_time_value(input.rising_v);
    return out;
}

LossTable from_gpu_loss_table(const IgbtGpuLossTable& table) {
    LossTable out;
    out.num_temps = table.num_temps;
    out.num_windows = table.num_windows;
    out.temperatures = table.temperatures;
    out.losses = table.losses;
    return out;
}

TimeValue from_gpu_time_value(const IgbtGpuTimeValue& tv) {
    TimeValue out;
    out.t = tv.t;
    out.y = tv.y;
    return out;
}

DeviceInput from_gpu_device_input(const IgbtGpuDeviceInput& input) {
    DeviceInput out;
    out.fundamental_i = from_gpu_time_value(input.fundamental_i);
    out.rising_i = from_gpu_time_value(input.rising_i);
    out.falling_v = from_gpu_time_value(input.falling_v);
    out.falling_i = from_gpu_time_value(input.falling_i);
    out.rising_v = from_gpu_time_value(input.rising_v);
    return out;
}

int thermal_cpu_threads() {
    static int threads = [] {
        if (const char* env = std::getenv("TRACEPV_THERMAL_CPU_THREADS")) {
            int value = std::atoi(env);
            if (value > 0) {
                return value;
            }
        }
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) {
            hw = 8;
        }
        return static_cast<int>(std::min<unsigned int>(8, hw));
    }();
    return threads;
}

void enable_nested_thermal_parallelism() {
    // OpenMP control variables belong to the calling task. Do not protect
    // these calls with a process-wide once_flag: in a multi-GPU outer region
    // that would configure only the first GPU worker and leave its siblings
    // with serialized inner thermal loops.
    omp_set_dynamic(0);
    if (omp_get_max_active_levels() < 2) {
        omp_set_max_active_levels(2);
    }
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    std::size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        return "";
    }
    std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::vector<double> split_doubles(const std::string& line) {
    std::vector<double> out;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) {
        cell = trim(cell);
        if (!cell.empty()) {
            out.push_back(std::stod(cell));
        }
    }
    return out;
}

std::vector<double> read_vector_csv(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Cannot open " + path);
    }
    std::vector<double> values;
    std::string line;
    while (std::getline(in, line)) {
        auto row = split_doubles(line);
        values.insert(values.end(), row.begin(), row.end());
    }
    return values;
}

Grid2D read_grid(const std::string& dir, const std::string& prefix) {
    Grid2D g;
    g.x = read_vector_csv(dir + "/" + prefix + "i.csv");
    g.y = read_vector_csv(dir + "/" + prefix + "v.csv");
    g.x_n = static_cast<int>(g.x.size());
    g.y_n = static_cast<int>(g.y.size());

    std::ifstream in(dir + "/" + prefix + "E_table.csv");
    if (!in) {
        throw std::runtime_error("Cannot open " + dir + "/" + prefix + "E_table.csv");
    }
    std::string line;
    while (std::getline(in, line)) {
        auto row = split_doubles(line);
        if (row.empty()) {
            continue;
        }
        if (static_cast<int>(row.size()) != g.x_n) {
            throw std::runtime_error("Grid row size mismatch for " + prefix);
        }
        g.z.insert(g.z.end(), row.begin(), row.end());
    }
    if (static_cast<int>(g.z.size()) != g.x_n * g.y_n) {
        throw std::runtime_error("Grid size mismatch for " + prefix);
    }
    return g;
}

ParameterSet load_parameters(const std::string& dir) {
    ParameterSet p;
    p.vce_14 = read_grid(dir, "Vce_IGBT_1_4");
    p.eon_14 = read_grid(dir, "Eon_IGBT_1_4");
    p.eoff_14 = read_grid(dir, "Eoff_IGBT_1_4");
    p.vce_23 = read_grid(dir, "Vce_IGBT_2_3");
    p.eon_23 = read_grid(dir, "Eon_IGBT_2_3");
    p.eoff_23 = read_grid(dir, "Eoff_IGBT_2_3");
    p.vf_14 = read_grid(dir, "Vf_Diode_1_4");
    p.err_14 = read_grid(dir, "Err_Diode_1_4");
    p.vf_23 = read_grid(dir, "Vf_Diode_2_3");
    p.err_23 = read_grid(dir, "Err_Diode_2_3");
    return p;
}

const ParameterSet& cached_parameters(const std::string& dir) {
    static std::mutex mutex;
    static std::map<std::string, ParameterSet> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(dir);
    if (it == cache.end()) {
        it = cache.emplace(dir, load_parameters(dir)).first;
    }
    return it->second;
}

bool igbt_debug_enabled() {
    const char* env = std::getenv("TRACEPV_DEBUG_IGBT_THERMAL");
    return env && env[0] != '\0' && env[0] != '0';
}

struct ScalarStats {
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double rms = 0.0;
};

ScalarStats stats_for(const std::vector<double>& values) {
    ScalarStats stats{};
    if (values.empty()) {
        return stats;
    }
    stats.min = values.front();
    stats.max = values.front();
    double sum = 0.0;
    double sum_sq = 0.0;
    for (double value : values) {
        stats.min = std::min(stats.min, value);
        stats.max = std::max(stats.max, value);
        sum += value;
        sum_sq += value * value;
    }
    stats.mean = sum / static_cast<double>(values.size());
    stats.rms = std::sqrt(sum_sq / static_cast<double>(values.size()));
    return stats;
}

ScalarStats stats_for_loss(const LossTable& table) {
    return stats_for(table.losses);
}

void append_edges(DeviceInput& input, const std::vector<double>& t,
                  const std::vector<double>& current,
                  const std::vector<double>& voltage) {
    if (t.empty()) {
        return;
    }
    double max_i = 0.0;
    for (double i : current) {
        max_i = std::max(max_i, std::abs(i));
    }
    double threshold = std::max(1e-9, 0.01 * max_i);
    bool was_active = std::abs(current.front()) > threshold;
    if (was_active) {
        input.rising_i.t.push_back(t.front());
        input.rising_i.y.push_back(std::abs(current.front()));
        input.falling_v.t.push_back(t.front());
        input.falling_v.y.push_back(std::abs(voltage.front()));
    }
    for (std::size_t i = 1; i < t.size(); ++i) {
        bool active = std::abs(current[i]) > threshold;
        if (active && !was_active) {
            input.rising_i.t.push_back(t[i]);
            input.rising_i.y.push_back(std::abs(current[i]));
            input.falling_v.t.push_back(t[i]);
            input.falling_v.y.push_back(std::abs(voltage[i]));
        } else if (!active && was_active) {
            input.falling_i.t.push_back(t[i - 1]);
            input.falling_i.y.push_back(std::abs(current[i - 1]));
            input.rising_v.t.push_back(t[i - 1]);
            input.rising_v.y.push_back(std::abs(voltage[i - 1]));
        }
        was_active = active;
    }
    if (was_active) {
        input.falling_i.t.push_back(t.back());
        input.falling_i.y.push_back(std::abs(current.back()));
        input.rising_v.t.push_back(t.back());
        input.rising_v.y.push_back(std::abs(voltage.back()));
    }
    if (input.rising_i.t.empty()) {
        auto it = std::max_element(current.begin(), current.end(),
                                   [](double a, double b) { return std::abs(a) < std::abs(b); });
        std::size_t idx = static_cast<std::size_t>(it - current.begin());
        input.rising_i.t.push_back(t[idx]);
        input.rising_i.y.push_back(std::abs(current[idx]));
        input.falling_v.t.push_back(t[idx]);
        input.falling_v.y.push_back(std::abs(voltage[idx]));
        input.falling_i.t.push_back(t[idx]);
        input.falling_i.y.push_back(std::abs(current[idx]));
        input.rising_v.t.push_back(t[idx]);
        input.rising_v.y.push_back(std::abs(voltage[idx]));
    }
}

void sort_time_value(TimeValue& tv) {
    std::vector<std::size_t> idx(tv.t.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return tv.t[a] < tv.t[b]; });
    std::vector<double> t2(tv.t.size()), y2(tv.y.size());
    for (std::size_t i = 0; i < idx.size(); ++i) {
        t2[i] = tv.t[idx[i]];
        y2[i] = tv.y[idx[i]];
    }
    tv.t.swap(t2);
    tv.y.swap(y2);
}

void finalize_device_input(DeviceInput& in) {
    sort_time_value(in.fundamental_i);
    sort_time_value(in.rising_i);
    sort_time_value(in.falling_v);
    sort_time_value(in.falling_i);
    sort_time_value(in.rising_v);
}

std::array<DeviceInput, 4> build_three_level_inputs(const UnifiedOutputs& outputs,
                                                    const SimulationParameters& params) {
    std::array<DeviceInput, 4> devices; // T1, T2, D1, D2
    const int n = outputs.total_samples;
    std::vector<double> ia_samples;
    std::vector<double> vdc_samples;
    std::vector<double> phase_v_samples;
    std::vector<double> outer_v_samples;
    if (igbt_debug_enabled()) {
        ia_samples.reserve(static_cast<std::size_t>(n));
        vdc_samples.reserve(static_cast<std::size_t>(n));
        phase_v_samples.reserve(static_cast<std::size_t>(n));
        outer_v_samples.reserve(static_cast<std::size_t>(n));
    }
    for (int sample = 0; sample < n; ++sample) {
        const double* state = outputs.states.data() + static_cast<std::size_t>(sample) * params.num_states;
        double t = outputs.time_points[static_cast<std::size_t>(sample)];
        double ia = state[0];
        int sa = outputs.switching_states[static_cast<std::size_t>(sample) * 3];
        if (sa > 1) {
            sa -= 1;
        }
        double vdc = params.v_pv;
        if (params.model_stage == 2 && params.num_states >= 12) {
            vdc = state[10] + state[11];
        }
        double phase_v = 0.5 * static_cast<double>(sa) * vdc;
        bool outer = std::abs(sa) > 0;
        bool inner = !outer;
        double outer_current = outer ? std::abs(ia) : 0.0;
        double inner_current = inner ? std::abs(ia) : 0.0;
        double diode_outer_current = outer ? std::max(-ia, 0.0) : 0.0;
        double diode_inner_current = inner ? std::max(ia, 0.0) : 0.0;
        double outer_v = std::max(0.0, 0.5 * vdc - phase_v);
        double inner_v = std::max(0.0, 0.5 * vdc - std::abs(phase_v));

        if (igbt_debug_enabled()) {
            ia_samples.push_back(ia);
            vdc_samples.push_back(vdc);
            phase_v_samples.push_back(phase_v);
            outer_v_samples.push_back(outer_v);
        }

        devices[0].fundamental_i.t.push_back(t);
        devices[0].fundamental_i.y.push_back(outer_current);
        devices[1].fundamental_i.t.push_back(t);
        devices[1].fundamental_i.y.push_back(inner_current);
        devices[2].fundamental_i.t.push_back(t);
        devices[2].fundamental_i.y.push_back(diode_outer_current);
        devices[3].fundamental_i.t.push_back(t);
        devices[3].fundamental_i.y.push_back(diode_inner_current);

        devices[0].rising_v.y.push_back(outer_v); // temporary voltage samples below
        devices[1].rising_v.y.push_back(inner_v);
        devices[2].rising_v.y.push_back(outer_v);
        devices[3].rising_v.y.push_back(inner_v);
    }

    std::vector<double> t = outputs.time_points;
    std::vector<double> t1_i = devices[0].fundamental_i.y;
    std::vector<double> t2_i = devices[1].fundamental_i.y;
    std::vector<double> d1_i = devices[2].fundamental_i.y;
    std::vector<double> d2_i = devices[3].fundamental_i.y;
    std::vector<double> t1_v = devices[0].rising_v.y;
    std::vector<double> t2_v = devices[1].rising_v.y;
    std::vector<double> d1_v = devices[2].rising_v.y;
    std::vector<double> d2_v = devices[3].rising_v.y;
    for (auto& d : devices) {
        d.rising_v.t.clear();
        d.rising_v.y.clear();
    }
    append_edges(devices[0], t, t1_i, t1_v);
    append_edges(devices[1], t, t2_i, t2_v);
    append_edges(devices[2], t, d1_i, d1_v);
    append_edges(devices[3], t, d2_i, d2_v);
    for (auto& d : devices) {
        finalize_device_input(d);
    }
    if (igbt_debug_enabled()) {
        static std::once_flag debug_flag;
        std::call_once(debug_flag, [&] {
            const auto ia_stats = stats_for(ia_samples);
            const auto vdc_stats = stats_for(vdc_samples);
            const auto phase_stats = stats_for(phase_v_samples);
            const auto outer_v_stats = stats_for(outer_v_samples);
            tracepv::reporting::debug_output() << "DEBUG: IGBT reference input waveform:" << std::endl;
            tracepv::reporting::debug_output() << "  params: v_pv=" << params.v_pv
                      << " V, vdc_target=" << params.vdc_target
                      << " V, boost_duty=" << params.boost_duty
                      << ", vg_mag=" << params.vg_mag
                      << " V, samples=" << n
                      << ", num_states=" << params.num_states << std::endl;
            tracepv::reporting::debug_output() << "  ia range/rms: [" << ia_stats.min << ", " << ia_stats.max
                      << "] A, rms=" << ia_stats.rms << " A" << std::endl;
            tracepv::reporting::debug_output() << "  vdc range/mean: [" << vdc_stats.min << ", " << vdc_stats.max
                      << "] V, mean=" << vdc_stats.mean << " V" << std::endl;
            tracepv::reporting::debug_output() << "  phase_v range: [" << phase_stats.min << ", " << phase_stats.max << "] V" << std::endl;
            tracepv::reporting::debug_output() << "  outer_v range: [" << outer_v_stats.min << ", " << outer_v_stats.max << "] V" << std::endl;
            for (std::size_t d = 0; d < devices.size(); ++d) {
                const auto current_stats = stats_for(devices[d].fundamental_i.y);
                tracepv::reporting::debug_output() << "  device " << d
                          << " fundamental_i rms=" << current_stats.rms
                          << " A, max=" << current_stats.max
                          << " A, rise_edges=" << devices[d].rising_i.t.size()
                          << ", fall_edges=" << devices[d].falling_i.t.size() << std::endl;
            }
        });
    }
    return devices;
}

std::vector<std::pair<int, int>> window_ranges(const TimeValue& tv, double tavg, int num_windows) {
    std::vector<std::pair<int, int>> ranges;
    ranges.reserve(static_cast<std::size_t>(num_windows));
    for (int k = 0; k < num_windows; ++k) {
        double start = k * tavg - 1e-12;
        double end = (k + 1) * tavg - 1e-12;
        auto b = std::lower_bound(tv.t.begin(), tv.t.end(), start);
        auto e = std::lower_bound(tv.t.begin(), tv.t.end(), end);
        ranges.push_back({static_cast<int>(b - tv.t.begin()), static_cast<int>(e - tv.t.begin())});
    }
    return ranges;
}

double conduction_loss(const TimeValue& current, const Grid2D& v_model, double tj,
                       double tavg, int start, int end) {
    int count = end - start;
    if (count < 2) {
        return 0.0;
    }
    double e_sum = 0.0;
    for (int i = start; i < end - 1; ++i) {
        double dt = current.t[static_cast<std::size_t>(i + 1)] - current.t[static_cast<std::size_t>(i)];
        double cur = current.y[static_cast<std::size_t>(i)];
        e_sum += v_model.eval(cur, tj) * cur * dt;
    }
    double dt_last = current.t[static_cast<std::size_t>(end - 1)] - current.t[static_cast<std::size_t>(end - 2)];
    double cur_last = current.y[static_cast<std::size_t>(end - 1)];
    e_sum += v_model.eval(cur_last, tj) * cur_last * dt_last;
    return e_sum / tavg;
}

std::vector<double> conduction_loss_by_window_prefix(
    const TimeValue& current,
    const Grid2D& v_model,
    double tj,
    double tavg,
    const std::vector<std::pair<int, int>>& ranges) {
    const int n = static_cast<int>(std::min(current.t.size(), current.y.size()));
    std::vector<double> losses(ranges.size(), 0.0);
    if (n < 2) {
        return losses;
    }

    std::vector<double> sample_power(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const double cur = current.y[static_cast<std::size_t>(i)];
        sample_power[static_cast<std::size_t>(i)] = v_model.eval(cur, tj) * cur;
    }

    std::vector<double> prefix_energy(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i + 1 < n; ++i) {
        const double dt = current.t[static_cast<std::size_t>(i + 1)] -
                          current.t[static_cast<std::size_t>(i)];
        prefix_energy[static_cast<std::size_t>(i + 1)] =
            prefix_energy[static_cast<std::size_t>(i)] +
            sample_power[static_cast<std::size_t>(i)] * dt;
    }

    for (std::size_t k = 0; k < ranges.size(); ++k) {
        int start = std::max(0, std::min(ranges[k].first, n));
        int end = std::max(0, std::min(ranges[k].second, n));
        if (end - start < 2) {
            continue;
        }
        double e_sum = prefix_energy[static_cast<std::size_t>(end - 1)] -
                       prefix_energy[static_cast<std::size_t>(start)];
        const double dt_last = current.t[static_cast<std::size_t>(end - 1)] -
                               current.t[static_cast<std::size_t>(end - 2)];
        e_sum += sample_power[static_cast<std::size_t>(end - 1)] * dt_last;
        losses[k] = e_sum / tavg;
    }
    return losses;
}

std::vector<double> paired_switching_loss_by_window_prefix(
    const TimeValue& current,
    const TimeValue& voltage,
    const Grid2D& energy_model,
    double tj,
    double tavg,
    const std::vector<std::pair<int, int>>& current_ranges,
    const std::vector<std::pair<int, int>>& voltage_ranges) {
    std::vector<double> losses(current_ranges.size(), 0.0);
    const int n = static_cast<int>(std::min(current.y.size(), voltage.y.size()));
    if (n <= 0 || voltage_ranges.size() != current_ranges.size()) {
        return losses;
    }

    bool aligned_ranges = true;
    for (std::size_t k = 0; k < current_ranges.size(); ++k) {
        if (current_ranges[k].first != voltage_ranges[k].first ||
            current_ranges[k].second != voltage_ranges[k].second) {
            aligned_ranges = false;
            break;
        }
    }

    if (!aligned_ranges) {
        for (std::size_t k = 0; k < current_ranges.size(); ++k) {
            int n_pair = std::min(current_ranges[k].second - current_ranges[k].first,
                                  voltage_ranges[k].second - voltage_ranges[k].first);
            double sum = 0.0;
            for (int i = 0; i < n_pair; ++i) {
                double cur = current.y[static_cast<std::size_t>(current_ranges[k].first + i)];
                double volt = voltage.y[static_cast<std::size_t>(voltage_ranges[k].first + i)];
                sum += energy_model.eval(cur, tj) * volt / kRatedVoltage;
            }
            losses[k] = sum / tavg;
        }
        return losses;
    }

    std::vector<double> prefix(static_cast<std::size_t>(n + 1), 0.0);
    for (int i = 0; i < n; ++i) {
        double cur = current.y[static_cast<std::size_t>(i)];
        double volt = voltage.y[static_cast<std::size_t>(i)];
        prefix[static_cast<std::size_t>(i + 1)] =
            prefix[static_cast<std::size_t>(i)] +
            energy_model.eval(cur, tj) * volt / kRatedVoltage;
    }

    for (std::size_t k = 0; k < current_ranges.size(); ++k) {
        int start = std::max(0, std::min(current_ranges[k].first, n));
        int end = std::max(0, std::min(current_ranges[k].second, n));
        if (end > start) {
            losses[k] = (prefix[static_cast<std::size_t>(end)] -
                         prefix[static_cast<std::size_t>(start)]) / tavg;
        }
    }
    return losses;
}

LossTable compute_igbt_loss(const DeviceInput& in, const Grid2D& vce, const Grid2D& eon,
                            const Grid2D& eoff, double rgon, double rgoff, bool inner,
                            double tavg, double tsim) {
    enable_nested_thermal_parallelism();
    int num_windows = std::max(1, static_cast<int>(std::floor(tsim / tavg)));
    std::vector<double> temps;
    for (int t = 25; t <= 150; t += 5) {
        temps.push_back(t);
    }
    auto cond_ranges = window_ranges(in.fundamental_i, tavg, num_windows);
    auto rise_i_ranges = window_ranges(in.rising_i, tavg, num_windows);
    auto fall_v_ranges = window_ranges(in.falling_v, tavg, num_windows);
    auto fall_i_ranges = window_ranges(in.falling_i, tavg, num_windows);
    auto rise_v_ranges = window_ranges(in.rising_v, tavg, num_windows);

    LossTable table;
    table.num_temps = static_cast<int>(temps.size());
    table.num_windows = num_windows;
    table.temperatures = temps;
    table.losses.assign(static_cast<std::size_t>(table.num_temps * num_windows), 0.0);

    #pragma omp parallel for schedule(static) num_threads(thermal_cpu_threads()) if(table.num_temps >= 2)
    for (int j = 0; j < table.num_temps; ++j) {
            double tj = temps[static_cast<std::size_t>(j)];
            auto pcond_by_window = conduction_loss_by_window_prefix(
                in.fundamental_i, vce, tj, tavg, cond_ranges);
            auto pon_by_window = paired_switching_loss_by_window_prefix(
                in.rising_i, in.falling_v, eon, tj, tavg, rise_i_ranges, fall_v_ranges);
            auto poff_by_window = paired_switching_loss_by_window_prefix(
                in.falling_i, in.rising_v, eoff, tj, tavg, fall_i_ranges, rise_v_ranges);
            for (int k = 0; k < num_windows; ++k) {
            double pon = pon_by_window[static_cast<std::size_t>(k)];
            double poff = poff_by_window[static_cast<std::size_t>(k)];
            if (inner) {
                double xgon = rgon / 10.0;
                double xgoff = rgoff / 10.0;
                poff *= 16.12276 / (xgoff + 13.33362) + 0.07055394 * xgoff - 0.1956174;
                pon *= -24.79985 / (xgon + 3.157488) + 0.2659224 * xgon + 6.653987;
            } else {
                double xgon = rgon / 6.8;
                double xgoff = rgoff / 6.8;
                poff *= 116.5799376831 / (xgoff + 54.6872724553717) +
                        0.0356026580548811 * xgoff - 1.1261135920061;
                pon *= -496.825849965403 / (xgon + 19.4140545009492) -
                       0.332711842464142 * xgon + 25.6259606688032;
            }
            double pcond = pcond_by_window[static_cast<std::size_t>(k)];
            table.losses[static_cast<std::size_t>(j + k * table.num_temps)] = pcond + pon + poff;
            }
    }
    return table;
}

LossTable compute_diode_loss(const DeviceInput& in, const Grid2D& vf, const Grid2D& err,
                             double rgon, bool inner, double tavg, double tsim) {
    enable_nested_thermal_parallelism();
    int num_windows = std::max(1, static_cast<int>(std::floor(tsim / tavg)));
    std::vector<double> temps;
    for (int t = 25; t <= 150; t += 5) {
        temps.push_back(t);
    }
    auto cond_ranges = window_ranges(in.fundamental_i, tavg, num_windows);
    auto fall_i_ranges = window_ranges(in.falling_i, tavg, num_windows);
    auto rise_v_ranges = window_ranges(in.rising_v, tavg, num_windows);

    LossTable table;
    table.num_temps = static_cast<int>(temps.size());
    table.num_windows = num_windows;
    table.temperatures = temps;
    table.losses.assign(static_cast<std::size_t>(table.num_temps * num_windows), 0.0);

    #pragma omp parallel for schedule(static) num_threads(thermal_cpu_threads()) if(table.num_temps >= 2)
    for (int j = 0; j < table.num_temps; ++j) {
            double tj = temps[static_cast<std::size_t>(j)];
            auto pcond_by_window = conduction_loss_by_window_prefix(
                in.fundamental_i, vf, tj, tavg, cond_ranges);
            auto prr_by_window = paired_switching_loss_by_window_prefix(
                in.falling_i, in.rising_v, err, tj, tavg, fall_i_ranges, rise_v_ranges);
            for (int k = 0; k < num_windows; ++k) {
            double prr = prr_by_window[static_cast<std::size_t>(k)];
            if (inner) {
                double x = rgon / 10.0;
                prr *= 1.736545 / (x + 1.499951) - 0.00545 * x + 0.3115;
            } else {
                double x = rgon / 10.0;
                prr *= 1.736545 / (x + 1.499951) - 0.00545 * x + 0.3115;
            }
            double pcond = pcond_by_window[static_cast<std::size_t>(k)];
            table.losses[static_cast<std::size_t>(j + k * table.num_temps)] = pcond + prr;
            }
    }
    return table;
}

double loss_lookup_window(const LossTable& table, double tj, int k) {
    k = std::max(0, std::min(k, table.num_windows - 1));
    const double* col = table.losses.data() + static_cast<std::size_t>(k * table.num_temps);
    const auto& tg = table.temperatures;
    if (tj <= tg.front()) {
        return col[0];
    }
    if (tj >= tg.back()) {
        return col[table.num_temps - 1];
    }
    auto it = std::upper_bound(tg.begin(), tg.end(), tj);
    int i1 = static_cast<int>(it - tg.begin());
    int i0 = i1 - 1;
    double w = (tj - tg[static_cast<std::size_t>(i0)]) /
               (tg[static_cast<std::size_t>(i1)] - tg[static_cast<std::size_t>(i0)]);
    return (1.0 - w) * col[i0] + w * col[i1];
}

double loss_lookup(const LossTable& table, double tj, double t_s, double tavg) {
    int k = static_cast<int>(std::floor(std::fmod(std::max(t_s, 0.0), kLossCycle) / tavg));
    k = std::max(0, std::min(k, table.num_windows - 1));
    return loss_lookup_window(table, tj, k);
}

struct ThermalStepCoeffs {
    double ths_keep = 0.0;
    double ths_power = 0.0;
    double ths_amb = 0.0;
    double case_keep = 0.0;
    double case_power = 0.0;
    double case_upstream = 0.0;
    std::array<std::array<double, 4>, 4> node_keep{};
    std::array<std::array<double, 4>, 4> node_power{};
    std::array<std::array<double, 4>, 4> node_upstream{};
};

IgbtReferenceThermalResult run_thermal(const LossTable& l1, const LossTable& l2,
                                       const LossTable& l3, const LossTable& l4,
                                       double ambient, double loss_tavg,
                                       double thermal_dt, double t_end) {
    IgbtReferenceThermalResult out;
    thermal_dt = std::max(thermal_dt, loss_tavg);
    std::size_t n_total = static_cast<std::size_t>(std::llround(t_end / thermal_dt)) + 1;
    double ths = ambient;
    double tcase = ambient;
    std::array<std::array<double, 6>, 4> nodes{};
    for (auto& device_nodes : nodes) {
        device_nodes.fill(ambient);
    }
    const std::array<std::array<double, 4>, 4> r = {{
        {{0.1347, 0.2501, 0.08578, 0.02939}},
        {{0.1497, 0.414, 0.2195, 0.1168}},
        {{0.1561, 0.4349, 0.2333, 0.1257}},
        {{0.1855, 0.4012, 0.2224, 0.191}}}};
    const std::array<std::array<double, 4>, 4> c = {{
        {{0.5811, 0.08053, 0.04136, 0.01172}},
        {{0.587, 0.05586, 0.01219, 0.002754}},
        {{0.5643, 0.533, 0.0115, 0.0025}},
        {{0.5013, 0.07472, 0.0145, 0.001744}}}};

    ThermalStepCoeffs coeff;
    {
        const double denom_hs = 1.0 + thermal_dt / (0.003 * 2097.0);
        coeff.ths_keep = 1.0 / denom_hs;
        coeff.ths_power = (thermal_dt / 2097.0) / denom_hs;
        coeff.ths_amb = (ambient * thermal_dt / (0.003 * 2097.0)) / denom_hs;

        const double denom_case = 1.0 + thermal_dt / (0.1 * 3.0);
        coeff.case_keep = 1.0 / denom_case;
        coeff.case_power = (thermal_dt / 3.0) / denom_case;
        coeff.case_upstream = (thermal_dt / (0.1 * 3.0)) / denom_case;

        for (int d = 0; d < 4; ++d) {
            for (int i = 0; i < 4; ++i) {
                const double rc = r[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)] *
                                  c[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)];
                const double denom = 1.0 + thermal_dt / rc;
                coeff.node_keep[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)] = 1.0 / denom;
                coeff.node_power[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)] =
                    (thermal_dt / c[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)]) / denom;
                coeff.node_upstream[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)] =
                    (thermal_dt / rc) / denom;
            }
        }
    }

    std::vector<int> window_index(n_total > 0 ? n_total - 1 : 0);
    for (std::size_t k = 0; k + 1 < n_total; ++k) {
        double tw = std::fmod(static_cast<double>(k) * thermal_dt, kLossCycle);
        if (tw < 0.0) {
            tw += kLossCycle;
        }
        window_index[k] = static_cast<int>(std::floor(tw / loss_tavg));
    }

    if (igbt_debug_enabled()) {
        static std::once_flag debug_flag;
        std::call_once(debug_flag, [&] {
            const auto s1 = stats_for_loss(l1);
            const auto s2 = stats_for_loss(l2);
            const auto s3 = stats_for_loss(l3);
            const auto s4 = stats_for_loss(l4);
            tracepv::reporting::debug_output() << "DEBUG: IGBT reference loss table stats:" << std::endl;
            tracepv::reporting::debug_output() << "  loss_tavg=" << loss_tavg
                      << " s, thermal_dt=" << thermal_dt
                      << " s, t_end=" << t_end
                      << " s, ambient=" << ambient << " C" << std::endl;
            tracepv::reporting::debug_output() << "  IGBT1 loss range/mean: [" << s1.min << ", " << s1.max
                      << "] W, mean=" << s1.mean << " W" << std::endl;
            tracepv::reporting::debug_output() << "  IGBT2 loss range/mean: [" << s2.min << ", " << s2.max
                      << "] W, mean=" << s2.mean << " W" << std::endl;
            tracepv::reporting::debug_output() << "  Diode1 loss range/mean: [" << s3.min << ", " << s3.max
                      << "] W, mean=" << s3.mean << " W" << std::endl;
            tracepv::reporting::debug_output() << "  Diode2 loss range/mean: [" << s4.min << ", " << s4.max
                      << "] W, mean=" << s4.mean << " W" << std::endl;
            tracepv::reporting::debug_output() << "  Estimated inverter loss from table means: "
                      << 6.0 * (s1.mean + s2.mean + s3.mean + s4.mean) << " W" << std::endl;
        });
    }

    double total_loss_sum = 0.0;
    for (std::size_t k = 0; k + 1 < n_total; ++k) {
        int loss_window = window_index[k];
        std::array<double, 4> p = {
            loss_lookup_window(l1, nodes[0][5], loss_window),
            loss_lookup_window(l2, nodes[1][5], loss_window),
            loss_lookup_window(l3, nodes[2][5], loss_window),
            loss_lookup_window(l4, nodes[3][5], loss_window)};
        double pleg = 2.0 * (p[0] + p[1] + p[2] + p[3]);
        double ptotal = 3.0 * pleg;
        total_loss_sum += ptotal;

        ths = coeff.ths_keep * ths + coeff.ths_power * ptotal + coeff.ths_amb;
        tcase = coeff.case_keep * tcase + coeff.case_power * pleg + coeff.case_upstream * ths;
        for (int d = 0; d < 4; ++d) {
            nodes[static_cast<std::size_t>(d)][2] =
                coeff.node_keep[static_cast<std::size_t>(d)][0] * nodes[static_cast<std::size_t>(d)][2] +
                coeff.node_power[static_cast<std::size_t>(d)][0] * p[static_cast<std::size_t>(d)] +
                coeff.node_upstream[static_cast<std::size_t>(d)][0] * tcase;
            for (int i = 3; i < 6; ++i) {
                const int rc_idx = i - 2;
                nodes[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)] =
                    coeff.node_keep[static_cast<std::size_t>(d)][static_cast<std::size_t>(rc_idx)] *
                        nodes[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)] +
                    coeff.node_power[static_cast<std::size_t>(d)][static_cast<std::size_t>(rc_idx)] *
                        p[static_cast<std::size_t>(d)] +
                    coeff.node_upstream[static_cast<std::size_t>(d)][static_cast<std::size_t>(rc_idx)] *
                        nodes[static_cast<std::size_t>(d)][static_cast<std::size_t>(i - 1)];
            }
        }
    }

    out.valid = true;
    out.igbt1_temperature = nodes[0][5];
    out.igbt2_temperature = nodes[1][5];
    out.diode1_temperature = nodes[2][5];
    out.diode2_temperature = nodes[3][5];
    out.case_temperature = tcase;
    out.heatsink_temperature = ths;
    out.junction_temperature = std::max({out.igbt1_temperature, out.igbt2_temperature,
                                         out.diode1_temperature, out.diode2_temperature});
    out.average_total_loss = total_loss_sum / static_cast<double>(std::max<std::size_t>(1, n_total - 1));
    return out;
}

} // namespace

IgbtReferenceThermalResult calculate_igbt_reference_thermal(
    const UnifiedOutputs& outputs,
    const SimulationParameters& params,
    double ambient_temperature,
    const std::string& parameter_dir,
    double thermal_end_time_s,
    double thermal_step_s) {
    IgbtReferenceThermalResult result;
    try {
        if (params.topology_level != 3) {
            result.message = "Reference IGBT model currently expects the 3-level device grouping.";
            return result;
        }
        if (outputs.total_samples <= 2 || outputs.states.empty() || outputs.switching_states.empty()) {
            result.message = "Electrical output is empty.";
            return result;
        }
        double tavg = params.switching_frequency > 0.0 ? 1.0 / params.switching_frequency
                                                       : 1.0 / kDefaultSwitchingFrequency;
        double tsim = outputs.time_points.empty() ? params.simulation_time : outputs.time_points.back();
        tsim = std::max(tsim, tavg);

        const auto parameter_lookup_start = std::chrono::steady_clock::now();
        const ParameterSet& p = cached_parameters(parameter_dir);
        result.parameter_lookup_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - parameter_lookup_start).count();

        const auto input_build_start = std::chrono::steady_clock::now();
        auto devices = build_three_level_inputs(outputs, params);
        result.input_build_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - input_build_start).count();

        const auto loss_igbt1_start = std::chrono::steady_clock::now();
        LossTable l1 = compute_igbt_loss(devices[0], p.vce_14, p.eon_14, p.eoff_14,
                                         6.8, 6.8, false, tavg, tsim);
        result.loss_igbt1_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_igbt1_start).count();

        const auto loss_igbt2_start = std::chrono::steady_clock::now();
        LossTable l2 = compute_igbt_loss(devices[1], p.vce_23, p.eon_23, p.eoff_23,
                                         10.0, 10.0, true, tavg, tsim);
        result.loss_igbt2_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_igbt2_start).count();

        const auto loss_diode1_start = std::chrono::steady_clock::now();
        LossTable l3 = compute_diode_loss(devices[2], p.vf_14, p.err_14,
                                          6.6, false, tavg, tsim);
        result.loss_diode1_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_diode1_start).count();

        const auto loss_diode2_start = std::chrono::steady_clock::now();
        LossTable l4 = compute_diode_loss(devices[3], p.vf_23, p.err_23,
                                          10.0, true, tavg, tsim);
        result.loss_diode2_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_diode2_start).count();

        const double parameter_lookup_s = result.parameter_lookup_s;
        const double input_build_s = result.input_build_s;
        const double loss_igbt1_s = result.loss_igbt1_s;
        const double loss_igbt2_s = result.loss_igbt2_s;
        const double loss_diode1_s = result.loss_diode1_s;
        const double loss_diode2_s = result.loss_diode2_s;

        const auto thermal_rc_start = std::chrono::steady_clock::now();
        result = run_thermal(l1, l2, l3, l4, ambient_temperature, tavg,
                             thermal_step_s > 0.0 ? thermal_step_s : tavg,
                             std::max(thermal_end_time_s, tavg));
        result.thermal_rc_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - thermal_rc_start).count();
        result.parameter_lookup_s = parameter_lookup_s;
        result.input_build_s = input_build_s;
        result.loss_igbt1_s = loss_igbt1_s;
        result.loss_igbt2_s = loss_igbt2_s;
        result.loss_diode1_s = loss_diode1_s;
        result.loss_diode2_s = loss_diode2_s;
        result.message = "reference IGBT loss/thermal model";
    } catch (const std::exception& ex) {
        result.valid = false;
        result.message = ex.what();
    }
    return result;
}

bool build_igbt_reference_cached_input(
    const UnifiedOutputs& outputs,
    const SimulationParameters& params,
    std::array<IgbtGpuDeviceInput, 4>& devices,
    double& tavg,
    double& tsim,
    std::string& message) {
    if (params.topology_level != 3) {
        message = "Reference IGBT model currently expects the 3-level device grouping.";
        return false;
    }
    if (outputs.total_samples <= 2 || outputs.states.empty() || outputs.switching_states.empty()) {
        message = "Electrical output is empty.";
        return false;
    }
    tavg = params.switching_frequency > 0.0 ? 1.0 / params.switching_frequency
                                            : 1.0 / kDefaultSwitchingFrequency;
    tsim = outputs.time_points.empty() ? params.simulation_time : outputs.time_points.back();
    tsim = std::max(tsim, tavg);

    auto built_devices = build_three_level_inputs(outputs, params);
    for (int d = 0; d < 4; ++d) {
        devices[static_cast<std::size_t>(d)] =
            to_gpu_device_input(built_devices[static_cast<std::size_t>(d)]);
    }
    message.clear();
    return true;
}

std::vector<IgbtReferenceThermalResult> calculate_igbt_reference_thermal_batch_cached(
    const std::vector<IgbtReferenceCachedThermalInput>& inputs,
    const std::string& parameter_dir,
    double thermal_end_time_s,
    double thermal_step_s) {
    std::vector<IgbtReferenceThermalResult> results(inputs.size());
    if (inputs.empty()) {
        return results;
    }

    // This function is normally called from one outer worker per GPU. Ensure
    // every worker can create its own inner CPU thermal team, including when
    // the library is used without the simulator main entry point.
    enable_nested_thermal_parallelism();

    try {
        const auto parameter_lookup_start = std::chrono::steady_clock::now();
        const ParameterSet& p = cached_parameters(parameter_dir);
        const double parameter_lookup_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - parameter_lookup_start).count();

        std::vector<std::size_t> valid_indices;
        valid_indices.reserve(inputs.size());
        std::vector<std::array<IgbtGpuDeviceInput, 4>> gpu_devices;
        std::vector<double> tavg_by_valid;
        std::vector<double> tsim_by_valid;

        double reference_tavg = 0.0;
        double reference_tsim = 0.0;
        bool compatible_batch = true;

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto& input = inputs[i];
            if (!input.devices || input.tavg <= 0.0 || input.tsim <= 0.0) {
                results[i].message = "Missing cached IGBT reference input.";
                continue;
            }
            if (valid_indices.empty()) {
                reference_tavg = input.tavg;
                reference_tsim = input.tsim;
            } else if (std::abs(input.tavg - reference_tavg) >
                           std::max(1e-15, std::abs(reference_tavg) * 1e-9) ||
                       std::abs(input.tsim - reference_tsim) >
                           std::max(1e-12, std::abs(reference_tsim) * 1e-9)) {
                compatible_batch = false;
            }
            valid_indices.push_back(i);
            gpu_devices.push_back(*input.devices);
            tavg_by_valid.push_back(input.tavg);
            tsim_by_valid.push_back(input.tsim);
        }

        if (valid_indices.empty()) {
            return results;
        }

        results[valid_indices.front()].parameter_lookup_s = parameter_lookup_s;

        const auto run_single_cached = [&](std::size_t idx) {
            IgbtReferenceThermalResult result;
            const auto& input = inputs[idx];
            const auto& devices = *input.devices;

            const auto loss_igbt1_start = std::chrono::steady_clock::now();
            LossTable l1 = compute_igbt_loss(
                from_gpu_device_input(devices[0]), p.vce_14, p.eon_14, p.eoff_14,
                6.8, 6.8, false, input.tavg, input.tsim);
            result.loss_igbt1_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_igbt1_start).count();

            const auto loss_igbt2_start = std::chrono::steady_clock::now();
            LossTable l2 = compute_igbt_loss(
                from_gpu_device_input(devices[1]), p.vce_23, p.eon_23, p.eoff_23,
                10.0, 10.0, true, input.tavg, input.tsim);
            result.loss_igbt2_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_igbt2_start).count();

            const auto loss_diode1_start = std::chrono::steady_clock::now();
            LossTable l3 = compute_diode_loss(
                from_gpu_device_input(devices[2]), p.vf_14, p.err_14,
                6.6, false, input.tavg, input.tsim);
            result.loss_diode1_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_diode1_start).count();

            const auto loss_diode2_start = std::chrono::steady_clock::now();
            LossTable l4 = compute_diode_loss(
                from_gpu_device_input(devices[3]), p.vf_23, p.err_23,
                10.0, true, input.tavg, input.tsim);
            result.loss_diode2_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_diode2_start).count();

            const double loss_igbt1_s = result.loss_igbt1_s;
            const double loss_igbt2_s = result.loss_igbt2_s;
            const double loss_diode1_s = result.loss_diode1_s;
            const double loss_diode2_s = result.loss_diode2_s;
            const auto thermal_rc_start = std::chrono::steady_clock::now();
            result = run_thermal(l1, l2, l3, l4, input.ambient_temperature, input.tavg,
                                 thermal_step_s > 0.0 ? thermal_step_s : input.tavg,
                                 std::max(thermal_end_time_s, input.tavg));
            result.thermal_rc_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - thermal_rc_start).count();
            result.loss_igbt1_s = loss_igbt1_s;
            result.loss_igbt2_s = loss_igbt2_s;
            result.loss_diode1_s = loss_diode1_s;
            result.loss_diode2_s = loss_diode2_s;
            result.message = "reference IGBT cached loss/thermal model";
            return result;
        };

        if (!compatible_batch) {
            #pragma omp parallel for schedule(dynamic) num_threads(thermal_cpu_threads()) if(valid_indices.size() > 1)
            for (int jj = 0; jj < static_cast<int>(valid_indices.size()); ++jj) {
                const std::size_t idx = valid_indices[static_cast<std::size_t>(jj)];
                results[idx] = run_single_cached(idx);
            }
            return results;
        }

        const std::array<IgbtGpuGrid2D, 4> conduction_grids = {
            to_gpu_grid(p.vce_14),
            to_gpu_grid(p.vce_23),
            to_gpu_grid(p.vf_14),
            to_gpu_grid(p.vf_23)};
        const std::array<IgbtGpuGrid2D, 4> turn_on_or_recovery_grids = {
            to_gpu_grid(p.eon_14),
            to_gpu_grid(p.eon_23),
            to_gpu_grid(p.err_14),
            to_gpu_grid(p.err_23)};
        const std::array<IgbtGpuGrid2D, 2> turn_off_grids = {
            to_gpu_grid(p.eoff_14),
            to_gpu_grid(p.eoff_23)};

        std::vector<double> ambient_temperatures;
        ambient_temperatures.reserve(valid_indices.size());
        for (std::size_t idx : valid_indices) {
            ambient_temperatures.push_back(inputs[idx].ambient_temperature);
        }

        std::vector<IgbtGpuThermalResult> fused_thermal_results;
        std::array<double, 4> fused_loss_elapsed{};
        double fused_thermal_s = 0.0;
        const char* enable_fused_env = std::getenv("TRACEPV_ENABLE_IGBT_FUSED_GPU");
        const bool enable_fused_gpu = enable_fused_env &&
                                      enable_fused_env[0] != '\0' &&
                                      enable_fused_env[0] != '0';
        const bool fused_gpu_ok = enable_fused_gpu && run_igbt_loss_thermal_fused_gpu_batch(
            gpu_devices,
            conduction_grids,
            turn_on_or_recovery_grids,
            turn_off_grids,
            ambient_temperatures,
            reference_tavg,
            reference_tsim,
            thermal_step_s > 0.0 ? thermal_step_s : reference_tavg,
            std::max(thermal_end_time_s, reference_tavg),
            fused_thermal_results,
            fused_loss_elapsed,
            fused_thermal_s);

        const double share = 1.0 / static_cast<double>(valid_indices.size());
        if (fused_gpu_ok && fused_thermal_results.size() == valid_indices.size()) {
            for (std::size_t j = 0; j < valid_indices.size(); ++j) {
                const std::size_t idx = valid_indices[j];
                const auto parameter_s = results[idx].parameter_lookup_s;
                const auto& gpu = fused_thermal_results[j];
                results[idx].valid = gpu.valid;
                results[idx].junction_temperature = gpu.junction_temperature;
                results[idx].igbt1_temperature = gpu.igbt1_temperature;
                results[idx].igbt2_temperature = gpu.igbt2_temperature;
                results[idx].diode1_temperature = gpu.diode1_temperature;
                results[idx].diode2_temperature = gpu.diode2_temperature;
                results[idx].case_temperature = gpu.case_temperature;
                results[idx].heatsink_temperature = gpu.heatsink_temperature;
                results[idx].average_total_loss = gpu.average_total_loss;
                results[idx].parameter_lookup_s = parameter_s;
                results[idx].loss_igbt1_s = fused_loss_elapsed[0] * share;
                results[idx].loss_igbt2_s = fused_loss_elapsed[1] * share;
                results[idx].loss_diode1_s = fused_loss_elapsed[2] * share;
                results[idx].loss_diode2_s = fused_loss_elapsed[3] * share;
                results[idx].thermal_rc_s = fused_thermal_s * share;
                results[idx].message = "reference IGBT cached fused loss/thermal GPU batch model";
            }
            return results;
        }

        std::vector<std::array<IgbtGpuLossTable, 4>> gpu_loss_tables;
        std::array<double, 4> loss_elapsed{};
        const bool gpu_ok = compute_igbt_loss_tables_gpu_batch(
            gpu_devices,
            conduction_grids,
            turn_on_or_recovery_grids,
            turn_off_grids,
            reference_tavg,
            reference_tsim,
            gpu_loss_tables,
            loss_elapsed);

        if (!gpu_ok || gpu_loss_tables.size() != valid_indices.size()) {
            #pragma omp parallel for schedule(dynamic) num_threads(thermal_cpu_threads()) if(valid_indices.size() > 1)
            for (int jj = 0; jj < static_cast<int>(valid_indices.size()); ++jj) {
                const std::size_t idx = valid_indices[static_cast<std::size_t>(jj)];
                results[idx] = run_single_cached(idx);
            }
            return results;
        }

        const char* enable_rc_gpu_env = std::getenv("TRACEPV_ENABLE_IGBT_THERMAL_GPU");
        const bool enable_rc_gpu = enable_rc_gpu_env &&
                                   enable_rc_gpu_env[0] != '\0' &&
                                   enable_rc_gpu_env[0] != '0';
        std::vector<IgbtGpuThermalResult> gpu_thermal_results;
        double gpu_thermal_s = 0.0;
        const bool thermal_gpu_ok = enable_rc_gpu && run_igbt_thermal_gpu_batch(
            gpu_loss_tables,
            ambient_temperatures,
            reference_tavg,
            thermal_step_s > 0.0 ? thermal_step_s : reference_tavg,
            std::max(thermal_end_time_s, reference_tavg),
            gpu_thermal_results,
            gpu_thermal_s);

        if (thermal_gpu_ok && gpu_thermal_results.size() == valid_indices.size()) {
            for (std::size_t j = 0; j < valid_indices.size(); ++j) {
                const std::size_t idx = valid_indices[j];
                const auto parameter_s = results[idx].parameter_lookup_s;
                const auto& gpu = gpu_thermal_results[j];
                results[idx].valid = gpu.valid;
                results[idx].junction_temperature = gpu.junction_temperature;
                results[idx].igbt1_temperature = gpu.igbt1_temperature;
                results[idx].igbt2_temperature = gpu.igbt2_temperature;
                results[idx].diode1_temperature = gpu.diode1_temperature;
                results[idx].diode2_temperature = gpu.diode2_temperature;
                results[idx].case_temperature = gpu.case_temperature;
                results[idx].heatsink_temperature = gpu.heatsink_temperature;
                results[idx].average_total_loss = gpu.average_total_loss;
                results[idx].parameter_lookup_s = parameter_s;
                results[idx].loss_igbt1_s = loss_elapsed[0] * share;
                results[idx].loss_igbt2_s = loss_elapsed[1] * share;
                results[idx].loss_diode1_s = loss_elapsed[2] * share;
                results[idx].loss_diode2_s = loss_elapsed[3] * share;
                results[idx].thermal_rc_s = gpu_thermal_s * share;
                results[idx].message = "reference IGBT cached loss/thermal GPU batch model";
            }
        } else {
            #pragma omp parallel for schedule(dynamic) num_threads(thermal_cpu_threads()) if(valid_indices.size() > 1)
            for (int jj = 0; jj < static_cast<int>(valid_indices.size()); ++jj) {
                const std::size_t j = static_cast<std::size_t>(jj);
                const std::size_t idx = valid_indices[j];
                LossTable l1 = from_gpu_loss_table(gpu_loss_tables[j][0]);
                LossTable l2 = from_gpu_loss_table(gpu_loss_tables[j][1]);
                LossTable l3 = from_gpu_loss_table(gpu_loss_tables[j][2]);
                LossTable l4 = from_gpu_loss_table(gpu_loss_tables[j][3]);

                const double parameter_s = results[idx].parameter_lookup_s;
                const auto thermal_rc_start = std::chrono::steady_clock::now();
                auto thermal = run_thermal(l1, l2, l3, l4, inputs[idx].ambient_temperature, tavg_by_valid[j],
                                           thermal_step_s > 0.0 ? thermal_step_s : tavg_by_valid[j],
                                           std::max(thermal_end_time_s, tavg_by_valid[j]));
                thermal.thermal_rc_s =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - thermal_rc_start).count();
                thermal.parameter_lookup_s = parameter_s;
                thermal.loss_igbt1_s = loss_elapsed[0] * share;
                thermal.loss_igbt2_s = loss_elapsed[1] * share;
                thermal.loss_diode1_s = loss_elapsed[2] * share;
                thermal.loss_diode2_s = loss_elapsed[3] * share;
                thermal.message = "reference IGBT cached loss GPU batch/CPU thermal model";
                results[idx] = thermal;
            }
        }
    } catch (const std::exception& ex) {
        for (auto& result : results) {
            result.valid = false;
            result.message = ex.what();
        }
    }
    return results;
}

std::vector<IgbtReferenceThermalResult> calculate_igbt_reference_thermal_batch(
    const std::vector<IgbtReferenceThermalInput>& inputs,
    const std::string& parameter_dir,
    double thermal_end_time_s,
    double thermal_step_s) {
    std::vector<IgbtReferenceThermalResult> results(inputs.size());
    if (inputs.empty()) {
        return results;
    }

    // See the cached batch path above: the OpenMP setting is task-local, so
    // each GPU worker must establish it before entering nested CPU loops.
    enable_nested_thermal_parallelism();

    try {
        const auto parameter_lookup_start = std::chrono::steady_clock::now();
        const ParameterSet& p = cached_parameters(parameter_dir);
        const double parameter_lookup_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - parameter_lookup_start).count();

        std::vector<std::size_t> valid_indices;
        valid_indices.reserve(inputs.size());
        std::vector<std::array<IgbtGpuDeviceInput, 4>> gpu_devices;
        std::vector<double> tavg_by_valid;
        std::vector<double> tsim_by_valid;

        double reference_tavg = 0.0;
        double reference_tsim = 0.0;
        bool compatible_batch = true;

        std::vector<unsigned char> input_valid(inputs.size(), 0);
        std::vector<double> tavg_by_input(inputs.size(), 0.0);
        std::vector<double> tsim_by_input(inputs.size(), 0.0);
        std::vector<std::array<IgbtGpuDeviceInput, 4>> gpu_devices_by_input(inputs.size());

        #pragma omp parallel for schedule(dynamic) num_threads(thermal_cpu_threads()) if(inputs.size() > 1)
        for (int ii = 0; ii < static_cast<int>(inputs.size()); ++ii) {
            const std::size_t i = static_cast<std::size_t>(ii);
            const auto& input = inputs[i];
            if (!input.outputs || !input.params) {
                results[i].message = "Missing IGBT reference input.";
                continue;
            }
            const auto& outputs = *input.outputs;
            const auto& params = *input.params;
            if (params.topology_level != 3) {
                results[i].message = "Reference IGBT model currently expects the 3-level device grouping.";
                continue;
            }
            if (outputs.total_samples <= 2 || outputs.states.empty() || outputs.switching_states.empty()) {
                results[i].message = "Electrical output is empty.";
                continue;
            }

            const double tavg = params.switching_frequency > 0.0 ? 1.0 / params.switching_frequency
                                                                 : 1.0 / kDefaultSwitchingFrequency;
            double tsim = outputs.time_points.empty() ? params.simulation_time : outputs.time_points.back();
            tsim = std::max(tsim, tavg);
            const auto input_build_start = std::chrono::steady_clock::now();
            auto devices = build_three_level_inputs(outputs, params);
            results[i].input_build_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - input_build_start).count();

            std::array<IgbtGpuDeviceInput, 4> gpu_case_devices{};
            for (int d = 0; d < 4; ++d) {
                gpu_case_devices[static_cast<std::size_t>(d)] =
                    to_gpu_device_input(devices[static_cast<std::size_t>(d)]);
            }
            tavg_by_input[i] = tavg;
            tsim_by_input[i] = tsim;
            gpu_devices_by_input[i] = std::move(gpu_case_devices);
            input_valid[i] = 1;
        }

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (!input_valid[i]) {
                continue;
            }
            const double tavg = tavg_by_input[i];
            const double tsim = tsim_by_input[i];
            if (valid_indices.empty()) {
                reference_tavg = tavg;
                reference_tsim = tsim;
            } else if (std::abs(tavg - reference_tavg) > std::max(1e-15, std::abs(reference_tavg) * 1e-9) ||
                       std::abs(tsim - reference_tsim) > std::max(1e-12, std::abs(reference_tsim) * 1e-9)) {
                compatible_batch = false;
            }

            valid_indices.push_back(i);
            gpu_devices.push_back(std::move(gpu_devices_by_input[i]));
            tavg_by_valid.push_back(tavg);
            tsim_by_valid.push_back(tsim);
        }

        if (valid_indices.empty()) {
            return results;
        }

        results[valid_indices.front()].parameter_lookup_s = parameter_lookup_s;

        if (!compatible_batch) {
            for (std::size_t j = 0; j < valid_indices.size(); ++j) {
                const std::size_t idx = valid_indices[j];
                results[idx] = calculate_igbt_reference_thermal(
                    *inputs[idx].outputs,
                    *inputs[idx].params,
                    inputs[idx].ambient_temperature,
                    parameter_dir,
                    thermal_end_time_s,
                    thermal_step_s);
            }
            return results;
        }

        const std::array<IgbtGpuGrid2D, 4> conduction_grids = {
            to_gpu_grid(p.vce_14),
            to_gpu_grid(p.vce_23),
            to_gpu_grid(p.vf_14),
            to_gpu_grid(p.vf_23)};
        const std::array<IgbtGpuGrid2D, 4> turn_on_or_recovery_grids = {
            to_gpu_grid(p.eon_14),
            to_gpu_grid(p.eon_23),
            to_gpu_grid(p.err_14),
            to_gpu_grid(p.err_23)};
        const std::array<IgbtGpuGrid2D, 2> turn_off_grids = {
            to_gpu_grid(p.eoff_14),
            to_gpu_grid(p.eoff_23)};

        std::vector<double> ambient_temperatures;
        ambient_temperatures.reserve(valid_indices.size());
        for (std::size_t idx : valid_indices) {
            ambient_temperatures.push_back(inputs[idx].ambient_temperature);
        }

        std::vector<IgbtGpuThermalResult> fused_thermal_results;
        std::array<double, 4> fused_loss_elapsed{};
        double fused_thermal_s = 0.0;
        const char* enable_fused_env = std::getenv("TRACEPV_ENABLE_IGBT_FUSED_GPU");
        const bool enable_fused_gpu = enable_fused_env &&
                                      enable_fused_env[0] != '\0' &&
                                      enable_fused_env[0] != '0';
        const bool fused_gpu_ok = enable_fused_gpu && run_igbt_loss_thermal_fused_gpu_batch(
            gpu_devices,
            conduction_grids,
            turn_on_or_recovery_grids,
            turn_off_grids,
            ambient_temperatures,
            reference_tavg,
            reference_tsim,
            thermal_step_s > 0.0 ? thermal_step_s : reference_tavg,
            std::max(thermal_end_time_s, reference_tavg),
            fused_thermal_results,
            fused_loss_elapsed,
            fused_thermal_s);

        const double share = 1.0 / static_cast<double>(valid_indices.size());
        if (fused_gpu_ok && fused_thermal_results.size() == valid_indices.size()) {
            for (std::size_t j = 0; j < valid_indices.size(); ++j) {
                const std::size_t idx = valid_indices[j];
                const auto parameter_s = results[idx].parameter_lookup_s;
                const auto input_s = results[idx].input_build_s;
                const auto& gpu = fused_thermal_results[j];
                results[idx].valid = gpu.valid;
                results[idx].junction_temperature = gpu.junction_temperature;
                results[idx].igbt1_temperature = gpu.igbt1_temperature;
                results[idx].igbt2_temperature = gpu.igbt2_temperature;
                results[idx].diode1_temperature = gpu.diode1_temperature;
                results[idx].diode2_temperature = gpu.diode2_temperature;
                results[idx].case_temperature = gpu.case_temperature;
                results[idx].heatsink_temperature = gpu.heatsink_temperature;
                results[idx].average_total_loss = gpu.average_total_loss;
                results[idx].parameter_lookup_s = parameter_s;
                results[idx].input_build_s = input_s;
                results[idx].loss_igbt1_s = fused_loss_elapsed[0] * share;
                results[idx].loss_igbt2_s = fused_loss_elapsed[1] * share;
                results[idx].loss_diode1_s = fused_loss_elapsed[2] * share;
                results[idx].loss_diode2_s = fused_loss_elapsed[3] * share;
                results[idx].thermal_rc_s = fused_thermal_s * share;
                results[idx].message = "reference IGBT fused loss/thermal GPU batch model";
            }
            return results;
        }

        std::vector<std::array<IgbtGpuLossTable, 4>> gpu_loss_tables;
        std::array<double, 4> loss_elapsed{};
        const bool gpu_ok = compute_igbt_loss_tables_gpu_batch(
            gpu_devices,
            conduction_grids,
            turn_on_or_recovery_grids,
            turn_off_grids,
            reference_tavg,
            reference_tsim,
            gpu_loss_tables,
            loss_elapsed);

        if (!gpu_ok || gpu_loss_tables.size() != valid_indices.size()) {
            for (std::size_t j = 0; j < valid_indices.size(); ++j) {
                const std::size_t idx = valid_indices[j];
                results[idx] = calculate_igbt_reference_thermal(
                    *inputs[idx].outputs,
                    *inputs[idx].params,
                    inputs[idx].ambient_temperature,
                    parameter_dir,
                    thermal_end_time_s,
                    thermal_step_s);
            }
            return results;
        }

        const char* enable_rc_gpu_env = std::getenv("TRACEPV_ENABLE_IGBT_THERMAL_GPU");
        const bool enable_rc_gpu = enable_rc_gpu_env &&
                                   enable_rc_gpu_env[0] != '\0' &&
                                   enable_rc_gpu_env[0] != '0';
        std::vector<IgbtGpuThermalResult> gpu_thermal_results;
        double gpu_thermal_s = 0.0;
        const bool thermal_gpu_ok = enable_rc_gpu && run_igbt_thermal_gpu_batch(
            gpu_loss_tables,
            ambient_temperatures,
            reference_tavg,
            thermal_step_s > 0.0 ? thermal_step_s : reference_tavg,
            std::max(thermal_end_time_s, reference_tavg),
            gpu_thermal_results,
            gpu_thermal_s);

        if (thermal_gpu_ok && gpu_thermal_results.size() == valid_indices.size()) {
            for (std::size_t j = 0; j < valid_indices.size(); ++j) {
                const std::size_t idx = valid_indices[j];
                const auto parameter_s = results[idx].parameter_lookup_s;
                const auto input_s = results[idx].input_build_s;
                const auto& gpu = gpu_thermal_results[j];
                results[idx].valid = gpu.valid;
                results[idx].junction_temperature = gpu.junction_temperature;
                results[idx].igbt1_temperature = gpu.igbt1_temperature;
                results[idx].igbt2_temperature = gpu.igbt2_temperature;
                results[idx].diode1_temperature = gpu.diode1_temperature;
                results[idx].diode2_temperature = gpu.diode2_temperature;
                results[idx].case_temperature = gpu.case_temperature;
                results[idx].heatsink_temperature = gpu.heatsink_temperature;
                results[idx].average_total_loss = gpu.average_total_loss;
                results[idx].parameter_lookup_s = parameter_s;
                results[idx].input_build_s = input_s;
                results[idx].loss_igbt1_s = loss_elapsed[0] * share;
                results[idx].loss_igbt2_s = loss_elapsed[1] * share;
                results[idx].loss_diode1_s = loss_elapsed[2] * share;
                results[idx].loss_diode2_s = loss_elapsed[3] * share;
                results[idx].thermal_rc_s = gpu_thermal_s * share;
                results[idx].message = "reference IGBT loss/thermal GPU batch model";
            }
        } else {
            #pragma omp parallel for schedule(dynamic) num_threads(thermal_cpu_threads()) if(valid_indices.size() > 1)
            for (int jj = 0; jj < static_cast<int>(valid_indices.size()); ++jj) {
                const std::size_t j = static_cast<std::size_t>(jj);
                const std::size_t idx = valid_indices[j];
                LossTable l1 = from_gpu_loss_table(gpu_loss_tables[j][0]);
                LossTable l2 = from_gpu_loss_table(gpu_loss_tables[j][1]);
                LossTable l3 = from_gpu_loss_table(gpu_loss_tables[j][2]);
                LossTable l4 = from_gpu_loss_table(gpu_loss_tables[j][3]);

                const double parameter_s = results[idx].parameter_lookup_s;
                const double input_s = results[idx].input_build_s;
                const auto thermal_rc_start = std::chrono::steady_clock::now();
                auto thermal = run_thermal(l1, l2, l3, l4, inputs[idx].ambient_temperature, tavg_by_valid[j],
                                           thermal_step_s > 0.0 ? thermal_step_s : tavg_by_valid[j],
                                           std::max(thermal_end_time_s, tavg_by_valid[j]));
                thermal.thermal_rc_s =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - thermal_rc_start).count();
                thermal.parameter_lookup_s = parameter_s;
                thermal.input_build_s = input_s;
                thermal.loss_igbt1_s = loss_elapsed[0] * share;
                thermal.loss_igbt2_s = loss_elapsed[1] * share;
                thermal.loss_diode1_s = loss_elapsed[2] * share;
                thermal.loss_diode2_s = loss_elapsed[3] * share;
                thermal.message = "reference IGBT loss GPU batch/CPU thermal model";
                results[idx] = thermal;
            }
        }
    } catch (const std::exception& ex) {
        for (auto& result : results) {
            result.valid = false;
            result.message = ex.what();
        }
    }
    return results;
}
