#include "internal_conditions.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

namespace {

constexpr double kMinRh = 0.001;
constexpr double kMaxRh = 100.0;
constexpr double kMagnusA = 17.625;
constexpr double kMagnusB = 243.04;
constexpr double kMagnusEs0 = 6.1094;
constexpr double kDefaultRatedPowerW = 10000.0;
constexpr double kEwmaSpan = 20.0;
constexpr int kDewPointRollingWindow = 4 * 24 * 2;

double clamp(double value, double lo, double hi) {
    return std::min(std::max(value, lo), hi);
}

double finite_or(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

double saturation_vapor_pressure(double temperature_c) {
    return kMagnusEs0 * std::exp((kMagnusA * temperature_c) / (kMagnusB + temperature_c));
}

double dew_point_from_temperature_rh(double temperature_c, double rh_percent) {
    const double rh = clamp(finite_or(rh_percent, 0.0), kMinRh, kMaxRh);
    const double temperature = finite_or(temperature_c, 25.0);
    const double alpha = std::log(rh / 100.0) +
                         (kMagnusA * temperature) / (kMagnusB + temperature);
    return (kMagnusB * alpha) / (kMagnusA - alpha);
}

double estimate_inverter_waste_heat(double power_ratio) {
    const double ratio = clamp(finite_or(power_ratio, 0.0), 0.0, 1.5);
    const double efficiency = clamp(0.98 - 0.08 * std::pow(1.0 - ratio, 4.0), 0.85, 0.99);
    const double power_out = ratio * kDefaultRatedPowerW;
    return power_out / efficiency - power_out;
}

double infer_rated_load(const std::vector<double>& load_values, double requested_rated) {
    if (requested_rated > 0.0 && std::isfinite(requested_rated)) {
        return requested_rated;
    }
    double max_load = 0.0;
    for (double load : load_values) {
        if (std::isfinite(load) && load > max_load) {
            max_load = load;
        }
    }
    return max_load > 0.0 ? max_load : 1.0;
}

} // namespace

double calculate_relative_humidity_from_temperature_dew_point(
    double temperature_c,
    double dew_point_c) {
    if (!std::isfinite(temperature_c) || !std::isfinite(dew_point_c) ||
        temperature_c <= -kMagnusB || dew_point_c <= -kMagnusB) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double es_t = saturation_vapor_pressure(temperature_c);
    const double es_td = saturation_vapor_pressure(dew_point_c);
    if (es_t <= 0.0 || !std::isfinite(es_t) || !std::isfinite(es_td)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return clamp(100.0 * (es_td / es_t), 0.0, 100.0);
}

void calculate_internal_conditions_ddm(
    const std::vector<double>& ambient_temps,
    const std::vector<double>& ambient_rhs,
    const std::vector<double>& load_values,
    double rated_load_value,
    std::vector<double>& internal_temps,
    std::vector<double>& internal_rhs,
    std::vector<double>* internal_dew_points) {
    const std::size_t num_cases = ambient_temps.size();
    if (ambient_rhs.size() != num_cases || load_values.size() != num_cases) {
        internal_temps.clear();
        internal_rhs.clear();
        if (internal_dew_points != nullptr) {
            internal_dew_points->clear();
        }
        return;
    }

    internal_temps.resize(num_cases);
    internal_rhs.resize(num_cases);
    if (internal_dew_points != nullptr) {
        internal_dew_points->resize(num_cases);
    }

    const double rated_load = infer_rated_load(load_values, rated_load_value);
    const double alpha = 2.0 / (kEwmaSpan + 1.0);
    double ewma_num = 0.0;
    double ewma_den = 0.0;
    std::deque<double> dew_window;
    double dew_window_sum = 0.0;

    for (std::size_t i = 0; i < num_cases; ++i) {
        const double ambient_temp = finite_or(ambient_temps[i], 25.0);
        const double ambient_rh = clamp(finite_or(ambient_rhs[i], 0.0), 0.0, 100.0);
        const double load = std::max(0.0, finite_or(load_values[i], 0.0));
        const double power_ratio = rated_load > 0.0 ? load / rated_load : 0.0;

        const double waste_heat = estimate_inverter_waste_heat(power_ratio);
        ewma_num = waste_heat + (1.0 - alpha) * ewma_num;
        ewma_den = 1.0 + (1.0 - alpha) * ewma_den;
        const double heat_ewma = ewma_den > 0.0 ? ewma_num / ewma_den : waste_heat;

        const double internal_temp = clamp(ambient_temp + 0.05 * heat_ewma, -50.0, 100.0);
        internal_temps[i] = internal_temp;

        const double ambient_dew_point = dew_point_from_temperature_rh(ambient_temp, ambient_rh);
        dew_window.push_back(ambient_dew_point);
        dew_window_sum += ambient_dew_point;
        if (static_cast<int>(dew_window.size()) > kDewPointRollingWindow) {
            dew_window_sum -= dew_window.front();
            dew_window.pop_front();
        }

        const double internal_dew_point = dew_window_sum / static_cast<double>(dew_window.size());
        internal_rhs[i] = calculate_relative_humidity_from_temperature_dew_point(
            internal_temp,
            internal_dew_point);
        if (internal_dew_points != nullptr) {
            (*internal_dew_points)[i] = internal_dew_point;
        }
    }
}

void calculate_internal_conditions(
    const std::vector<double>& ambient_temps,
    const std::vector<double>& ambient_rhs,
    const std::vector<double>& load_values,
    std::vector<double>& internal_temps,
    std::vector<double>& internal_rhs) {
    calculate_internal_conditions_ddm(
        ambient_temps,
        ambient_rhs,
        load_values,
        0.0,
        internal_temps,
        internal_rhs
    );
}
