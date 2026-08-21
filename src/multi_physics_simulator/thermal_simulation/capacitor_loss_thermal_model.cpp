#include "capacitor_loss_thermal_model.h"
#include "capacitor_reference_gpu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <omp.h>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct EsrSample {
    double log_freq_khz = 0.0;
    double temperature = 0.0;
    double log_esr = 0.0;
};

struct EsrTable {
    std::vector<EsrSample> samples;
    std::vector<double> log_freq_khz_values;
    std::vector<double> temperature_values;
    std::vector<double> log_esr_values;
    double temp_min = 0.0;
    double temp_max = 0.0;
};

struct HarmonicBasis {
    std::size_t n = 0;
    std::size_t max_harmonic = 0;
    double duration = 0.0;
    std::vector<double> cos_values;
    std::vector<double> sin_values;
};

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

EsrTable load_esr_table(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Cannot open " + path);
    }
    EsrTable table;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto f = split_csv(line);
        if (f.size() < 3) {
            continue;
        }
        double freq_khz = std::stod(f[0]);
        double esr = std::stod(f[1]);
        double temp = std::stod(f[2]);
        if (freq_khz > 0.0 && esr > 0.0 && std::isfinite(temp)) {
            const double log_freq = std::log10(freq_khz);
            const double log_esr = std::log10(esr);
            table.samples.push_back({log_freq, temp, log_esr});
            table.log_freq_khz_values.push_back(log_freq);
            table.temperature_values.push_back(temp);
            table.log_esr_values.push_back(log_esr);
        }
    }
    if (table.samples.empty()) {
        throw std::runtime_error("No ESR samples loaded from " + path);
    }
    auto temp_minmax = std::minmax_element(
        table.samples.begin(),
        table.samples.end(),
        [](const EsrSample& a, const EsrSample& b) {
            return a.temperature < b.temperature;
        }
    );
    table.temp_min = temp_minmax.first->temperature;
    table.temp_max = temp_minmax.second->temperature;
    return table;
}

const EsrTable& cached_esr_table(const std::string& path) {
    static std::mutex mutex;
    static std::map<std::string, EsrTable> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(path);
    if (it == cache.end()) {
        it = cache.emplace(path, load_esr_table(path)).first;
    }
    return it->second;
}

double esr_at(const EsrTable& table, double freq_hz, double temp_c) {
    double x = std::log10(std::max(freq_hz / 1000.0, 1e-12));
    double y = std::clamp(temp_c, table.temp_min, table.temp_max);
    const auto& samples = table.samples;
    std::vector<std::pair<double, std::size_t>> distances;
    distances.reserve(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        double dx = x - samples[i].log_freq_khz;
        double dy = (y - samples[i].temperature) / 60.0;
        double d2 = dx * dx + dy * dy;
        if (d2 < 1e-24) {
            return std::pow(10.0, samples[i].log_esr);
        }
        distances.push_back({d2, i});
    }
    constexpr std::size_t k_nearest = 12;
    std::size_t count = std::min(k_nearest, distances.size());
    std::nth_element(distances.begin(), distances.begin() + static_cast<std::ptrdiff_t>(count - 1), distances.end());
    double num = 0.0;
    double den = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        double w = 1.0 / distances[i].first;
        num += w * samples[distances[i].second].log_esr;
        den += w;
    }
    return std::pow(10.0, num / den);
}

bool is_uniform_time_grid(const std::vector<double>& time, std::size_t n, double duration) {
    if (n < 4 || duration <= 0.0) {
        return false;
    }
    const double dt = duration / static_cast<double>(n - 1);
    const double tolerance = std::max(1e-12, std::abs(dt) * 1e-6);
    for (std::size_t i = 1; i < n; ++i) {
        const double expected = time[0] + static_cast<double>(i) * dt;
        if (std::abs(time[i] - expected) > tolerance) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<const HarmonicBasis> cached_harmonic_basis(std::size_t n,
                                                           std::size_t max_harmonic,
                                                           double duration) {
    static std::mutex mutex;
    static std::map<std::tuple<std::size_t, std::size_t, long long>,
                    std::shared_ptr<const HarmonicBasis>> cache;
    const long long duration_key = static_cast<long long>(std::llround(duration * 1e12));
    const auto key = std::make_tuple(n, max_harmonic, duration_key);
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
    }

    auto basis = std::make_shared<HarmonicBasis>();
    basis->n = n;
    basis->max_harmonic = max_harmonic;
    basis->duration = duration;
    basis->cos_values.resize(max_harmonic * n);
    basis->sin_values.resize(max_harmonic * n);
    for (std::size_t h = 1; h <= max_harmonic; ++h) {
        const std::size_t offset = (h - 1) * n;
        for (std::size_t i = 0; i < n; ++i) {
            const double phase = 2.0 * kPi * static_cast<double>(h) *
                                 static_cast<double>(i) / static_cast<double>(n - 1);
            basis->cos_values[offset + i] = std::cos(phase);
            basis->sin_values[offset + i] = std::sin(phase);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        cache[key] = basis;
    }
    return basis;
}

std::vector<std::pair<double, double>> harmonic_rms(const std::vector<double>& time,
                                                    const std::vector<double>& current) {
    const std::size_t n = std::min(time.size(), current.size());
    if (n < 4) {
        return {};
    }
    double duration = time[n - 1] - time[0];
    if (duration <= 0.0) {
        return {};
    }
    double time_domain_sum_sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        time_domain_sum_sq += current[i] * current[i];
    }
    const double time_domain_rms = std::sqrt(time_domain_sum_sq / static_cast<double>(n));

    double fundamental = 1.0 / duration;
    std::size_t max_harmonic = std::min<std::size_t>(200, n / 2);
    std::vector<std::pair<double, double>> harmonics;
    harmonics.reserve(max_harmonic);
    double harmonic_rms_sq = 0.0;

    std::shared_ptr<const HarmonicBasis> basis;
    if (is_uniform_time_grid(time, n, duration)) {
        basis = cached_harmonic_basis(n, max_harmonic, duration);
    }

    for (std::size_t h = 1; h <= max_harmonic; ++h) {
        double a = 0.0;
        double b = 0.0;
        double freq = static_cast<double>(h) * fundamental;
        if (basis) {
            const std::size_t offset = (h - 1) * n;
            const double* cos_values = basis->cos_values.data() + offset;
            const double* sin_values = basis->sin_values.data() + offset;
            for (std::size_t i = 0; i < n; ++i) {
                a += current[i] * cos_values[i];
                b += current[i] * sin_values[i];
            }
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                double phase = 2.0 * kPi * freq * (time[i] - time[0]);
                a += current[i] * std::cos(phase);
                b += current[i] * std::sin(phase);
            }
        }
        a *= 2.0 / static_cast<double>(n);
        b *= 2.0 / static_cast<double>(n);
        double peak = std::sqrt(a * a + b * b);
        double rms = peak / std::sqrt(2.0);
        if (rms > 1e-9) {
            harmonics.push_back({freq, rms});
            harmonic_rms_sq += rms * rms;
        }
    }
    const double time_domain_rms_sq = time_domain_rms * time_domain_rms;
    if (harmonic_rms_sq > time_domain_rms_sq * 1.05 && harmonic_rms_sq > 0.0) {
        const double scale = time_domain_rms / std::sqrt(harmonic_rms_sq);
        for (auto& harmonic : harmonics) {
            harmonic.second *= scale;
        }
    }
    return harmonics;
}

double loss_at_temperature(const std::vector<std::pair<double, double>>& harmonics,
                           const EsrTable& esr_table,
                           double temp_c) {
    double loss = 0.0;
    for (const auto& [freq, irms] : harmonics) {
        loss += irms * irms * esr_at(esr_table, freq, temp_c);
    }
    return loss;
}

double polyval(const std::vector<double>& coeff_desc, double x) {
    double y = 0.0;
    for (double c : coeff_desc) {
        y = y * x + c;
    }
    return y;
}

std::vector<double> solve_linear(std::vector<std::vector<double>> a, std::vector<double> b) {
    const std::size_t n = b.size();
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < n; ++row) {
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) {
                pivot = row;
            }
        }
        if (std::abs(a[pivot][col]) < 1e-18) {
            throw std::runtime_error("Singular capacitor loss polynomial fit matrix");
        }
        std::swap(a[pivot], a[col]);
        std::swap(b[pivot], b[col]);
        double div = a[col][col];
        for (std::size_t j = col; j < n; ++j) {
            a[col][j] /= div;
        }
        b[col] /= div;
        for (std::size_t row = 0; row < n; ++row) {
            if (row == col) {
                continue;
            }
            double factor = a[row][col];
            for (std::size_t j = col; j < n; ++j) {
                a[row][j] -= factor * a[col][j];
            }
            b[row] -= factor * b[col];
        }
    }
    return b;
}

std::vector<double> polyfit_desc(const std::vector<double>& x,
                                 const std::vector<double>& y,
                                 int order) {
    const std::size_t ncoef = static_cast<std::size_t>(order + 1);
    std::vector<std::vector<double>> ata(ncoef, std::vector<double>(ncoef, 0.0));
    std::vector<double> aty(ncoef, 0.0);
    for (std::size_t r = 0; r < x.size(); ++r) {
        std::vector<double> row(ncoef, 1.0);
        for (int p = order; p >= 0; --p) {
            row[static_cast<std::size_t>(order - p)] = std::pow(x[r], p);
        }
        for (std::size_t i = 0; i < ncoef; ++i) {
            aty[i] += row[i] * y[r];
            for (std::size_t j = 0; j < ncoef; ++j) {
                ata[i][j] += row[i] * row[j];
            }
        }
    }
    return solve_linear(ata, aty);
}

double cached_grid_esr(const EsrTable& esr_table,
                       const std::string& table_path,
                       double freq_hz,
                       double temp_c) {
    static std::mutex mutex;
    static std::map<std::tuple<std::string, long long, long long>, double> cache;
    long long freq_key = static_cast<long long>(std::llround(freq_hz * 1000.0));
    long long temp_key = static_cast<long long>(std::llround(temp_c * 1000.0));
    auto key = std::make_tuple(table_path, freq_key, temp_key);
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
    }
    double value = esr_at(esr_table, freq_hz, temp_c);
    {
        std::lock_guard<std::mutex> lock(mutex);
        cache.emplace(std::move(key), value);
    }
    return value;
}

double loss_at_temperature_cached(const std::vector<std::pair<double, double>>& harmonics,
                                  const EsrTable& esr_table,
                                  const std::string& table_path,
                                  double temp_c) {
    double loss = 0.0;
    for (const auto& [freq, irms] : harmonics) {
        loss += irms * irms * cached_grid_esr(esr_table, table_path, freq, temp_c);
    }
    return loss;
}

std::vector<std::vector<double>> build_case_esr_grid(
    const std::vector<std::pair<double, double>>& harmonics,
    const EsrTable& esr_table,
    const std::vector<double>& temps) {
    std::vector<std::vector<double>> grid(harmonics.size(), std::vector<double>(temps.size(), 0.0));
    #pragma omp parallel for schedule(static) if(harmonics.size() * temps.size() >= 128)
    for (int h = 0; h < static_cast<int>(harmonics.size()); ++h) {
        for (std::size_t t = 0; t < temps.size(); ++t) {
            grid[static_cast<std::size_t>(h)][t] =
                esr_at(esr_table, harmonics[static_cast<std::size_t>(h)].first, temps[t]);
        }
    }
    return grid;
}

std::vector<double> loss_grid_from_esr_grid(
    const std::vector<std::pair<double, double>>& harmonics,
    const std::vector<std::vector<double>>& esr_grid,
    const std::vector<double>& temps) {
    std::vector<double> losses(temps.size(), 0.0);
    for (std::size_t t = 0; t < temps.size(); ++t) {
        double loss = 0.0;
        for (std::size_t h = 0; h < harmonics.size(); ++h) {
            const double irms = harmonics[h].second;
            loss += irms * irms * esr_grid[h][t];
        }
        losses[t] = loss;
    }
    return losses;
}

void finalize_capacitor_reference_thermal(
    const std::vector<std::pair<double, double>>& harmonics,
    const std::vector<std::vector<double>>& esr_grid,
    const std::vector<double>& temps,
    const EsrTable& esr_table,
    double ambient_temperature,
    double rth_surface_ambient,
    double rth_core_surface,
    CapacitorReferenceThermalResult& result) {
    const auto loss_grid_start = std::chrono::steady_clock::now();
    auto losses = loss_grid_from_esr_grid(harmonics, esr_grid, temps);
    result.loss_grid_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_grid_start).count();

    const auto polyfit_start = std::chrono::steady_clock::now();
    std::vector<double> log_losses;
    log_losses.reserve(losses.size());
    for (double loss : losses) {
        log_losses.push_back(std::log(std::max(loss, 1e-300)));
    }
    std::vector<double> coeff = polyfit_desc(temps, log_losses, 6);
    result.polyfit_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - polyfit_start).count();

    const auto thermal_iteration_start = std::chrono::steady_clock::now();
    constexpr int max_iter = 200;
    constexpr double tol = 1e-3;
    constexpr double damp = 0.2;
    double th = ambient_temperature;
    double ts = ambient_temperature;
    double p = 0.0;
    double p_ambient = loss_at_temperature(harmonics, esr_table, ambient_temperature);
    for (int iter = 0; iter < max_iter; ++iter) {
        p = std::exp(polyval(coeff, th));
        double ts_new = ambient_temperature + p * rth_surface_ambient;
        double th_new = ts_new + p * rth_core_surface;
        double ts_upd = (1.0 - damp) * ts + damp * ts_new;
        double th_upd = (1.0 - damp) * th + damp * th_new;
        if (std::max(std::abs(ts_upd - ts), std::abs(th_upd - th)) < tol) {
            ts = ts_upd;
            th = th_upd;
            break;
        }
        ts = ts_upd;
        th = th_upd;
    }
    result.thermal_iteration_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - thermal_iteration_start).count();

    const double rth_total = rth_surface_ambient + rth_core_surface;
    const double fallback_surface = ambient_temperature + p_ambient * rth_surface_ambient;
    const double fallback_hotspot = ambient_temperature + p_ambient * rth_total;
    if (!std::isfinite(p) || !std::isfinite(th) || !std::isfinite(ts) ||
        th < ambient_temperature - 1e-6 ||
        std::abs(th - ambient_temperature) > std::max(100.0, 5.0 * std::abs(fallback_hotspot - ambient_temperature))) {
        p = p_ambient;
        ts = fallback_surface;
        th = fallback_hotspot;
        result.message = "harmonic ESR capacitor loss/static thermal model (iteration clamped)";
    } else {
        result.message = "harmonic ESR capacitor loss/cached polynomial thermal model";
    }

    result.valid = true;
    result.loss = p;
    result.hotspot_temperature = th;
    result.surface_temperature = ts;
}

} // namespace

CapacitorReferenceThermalResult calculate_capacitor_reference_thermal(
    const std::vector<double>& time_points,
    const std::vector<double>& capacitor_current,
    double ambient_temperature,
    double rth_surface_ambient,
    double rth_core_surface,
    const std::string& esr_table_path) {
    CapacitorReferenceThermalResult result;
    try {
        const auto& esr_table = cached_esr_table(esr_table_path);
        const auto harmonic_start = std::chrono::steady_clock::now();
        std::vector<std::pair<double, double>> harmonics;
        double gpu_harmonic_s = 0.0;
        bool harmonic_gpu_used = calculate_capacitor_harmonics_gpu(
            time_points, capacitor_current, harmonics, gpu_harmonic_s);
        if (harmonic_gpu_used) {
            result.harmonic_extraction_s = gpu_harmonic_s;
        } else {
            harmonics = harmonic_rms(time_points, capacitor_current);
            result.harmonic_extraction_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - harmonic_start).count();
        }
        if (harmonics.empty()) {
            result.message = "No capacitor harmonics could be extracted.";
            return result;
        }

        std::vector<double> temps;
        temps.reserve(26);
        for (int temp = -25; temp <= 100; temp += 5) {
            temps.push_back(static_cast<double>(temp));
        }

        const auto esr_grid_start = std::chrono::steady_clock::now();
        std::vector<std::vector<double>> esr_grid;
        double gpu_esr_s = 0.0;
        bool esr_gpu_used = calculate_capacitor_esr_grid_gpu(
            harmonics,
            temps,
            esr_table.log_freq_khz_values,
            esr_table.temperature_values,
            esr_table.log_esr_values,
            esr_table.temp_min,
            esr_table.temp_max,
            esr_grid,
            gpu_esr_s);
        if (esr_gpu_used) {
            result.esr_grid_s = gpu_esr_s;
        } else {
            esr_grid = build_case_esr_grid(harmonics, esr_table, temps);
            result.esr_grid_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - esr_grid_start).count();
        }

        finalize_capacitor_reference_thermal(
            harmonics,
            esr_grid,
            temps,
            esr_table,
            ambient_temperature,
            rth_surface_ambient,
            rth_core_surface,
            result);
    } catch (const std::exception& ex) {
        result.valid = false;
        result.message = ex.what();
    }
    return result;
}

std::vector<CapacitorReferenceThermalResult> calculate_capacitor_reference_thermal_batch(
    const std::vector<CapacitorReferenceThermalInput>& inputs,
    const std::string& esr_table_path) {
    std::vector<CapacitorReferenceThermalResult> results(inputs.size());
    if (inputs.empty()) {
        return results;
    }

    try {
        const auto& esr_table = cached_esr_table(esr_table_path);
        std::vector<double> temps;
        temps.reserve(26);
        for (int temp = -25; temp <= 100; temp += 5) {
            temps.push_back(static_cast<double>(temp));
        }

        std::vector<std::vector<std::pair<double, double>>> batch_harmonics(inputs.size());
        std::vector<std::size_t> valid_indices;
        valid_indices.reserve(inputs.size());

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto& input = inputs[i];
            if (!input.time_points || !input.capacitor_current) {
                results[i].message = "Missing capacitor waveform input.";
                continue;
            }

            const auto harmonic_start = std::chrono::steady_clock::now();
            double gpu_harmonic_s = 0.0;
            bool harmonic_gpu_used = calculate_capacitor_harmonics_gpu(
                *input.time_points, *input.capacitor_current, batch_harmonics[i], gpu_harmonic_s);
            if (harmonic_gpu_used) {
                results[i].harmonic_extraction_s = gpu_harmonic_s;
            } else {
                batch_harmonics[i] = harmonic_rms(*input.time_points, *input.capacitor_current);
                results[i].harmonic_extraction_s =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - harmonic_start).count();
            }
            if (batch_harmonics[i].empty()) {
                results[i].message = "No capacitor harmonics could be extracted.";
            } else {
                valid_indices.push_back(i);
            }
        }

        if (valid_indices.empty()) {
            return results;
        }

        std::vector<std::vector<std::pair<double, double>>> valid_harmonics;
        valid_harmonics.reserve(valid_indices.size());
        for (std::size_t idx : valid_indices) {
            valid_harmonics.push_back(batch_harmonics[idx]);
        }

        const auto esr_grid_start = std::chrono::steady_clock::now();
        std::vector<std::vector<std::vector<double>>> valid_esr_grids;
        double gpu_esr_s = 0.0;
        bool esr_gpu_used = calculate_capacitor_esr_grids_gpu_batch(
            valid_harmonics,
            temps,
            esr_table.log_freq_khz_values,
            esr_table.temperature_values,
            esr_table.log_esr_values,
            esr_table.temp_min,
            esr_table.temp_max,
            valid_esr_grids,
            gpu_esr_s);

        if (esr_gpu_used && valid_esr_grids.size() == valid_indices.size()) {
            const double esr_share = gpu_esr_s / static_cast<double>(valid_indices.size());
            for (std::size_t j = 0; j < valid_indices.size(); ++j) {
                const std::size_t idx = valid_indices[j];
                results[idx].esr_grid_s = esr_share;
                finalize_capacitor_reference_thermal(
                    batch_harmonics[idx],
                    valid_esr_grids[j],
                    temps,
                    esr_table,
                    inputs[idx].ambient_temperature,
                    inputs[idx].rth_surface_ambient,
                    inputs[idx].rth_core_surface,
                    results[idx]);
            }
        } else {
            for (std::size_t idx : valid_indices) {
                const auto case_esr_start = std::chrono::steady_clock::now();
                auto esr_grid = build_case_esr_grid(batch_harmonics[idx], esr_table, temps);
                results[idx].esr_grid_s =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - case_esr_start).count();
                finalize_capacitor_reference_thermal(
                    batch_harmonics[idx],
                    esr_grid,
                    temps,
                    esr_table,
                    inputs[idx].ambient_temperature,
                    inputs[idx].rth_surface_ambient,
                    inputs[idx].rth_core_surface,
                    results[idx]);
            }
            if (!esr_gpu_used) {
                const double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - esr_grid_start).count();
                (void)elapsed;
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
