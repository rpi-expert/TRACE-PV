#include "multi_physics_simulator/environmental_simulation/internal_conditions.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool close_to(double actual, double expected, double tolerance = 1e-3) {
    return std::abs(actual - expected) <= tolerance;
}

} // namespace

int main() {
    require(close_to(
                calculate_relative_humidity_from_temperature_dew_point(20.0, 10.0),
                52.5413),
            "unexpected RH at 20 C for a 10 C dew point");
    require(close_to(
                calculate_relative_humidity_from_temperature_dew_point(30.0, 10.0),
                28.9384),
            "provided temperature must preserve dew point when recomputing RH");
    require(calculate_relative_humidity_from_temperature_dew_point(5.0, 10.0) ==
                100.0,
            "temperature at or below dew point must clamp RH to 100 percent");
    require(std::isnan(
                calculate_relative_humidity_from_temperature_dew_point(
                    std::nan(""),
                    10.0)),
            "non-finite temperature must not silently become a physical RH");
    require(std::isnan(
                calculate_relative_humidity_from_temperature_dew_point(
                    -243.04,
                    10.0)) &&
                std::isnan(
                    calculate_relative_humidity_from_temperature_dew_point(
                        20.0,
                        -243.04)),
            "Magnus denominator boundary must return NaN");

    std::vector<double> internal_temps;
    std::vector<double> internal_rhs;
    std::vector<double> internal_dew_points;
    calculate_internal_conditions_ddm(
        {20.0, 21.0},
        {50.0, 55.0},
        {0.0, 5000.0},
        10000.0,
        internal_temps,
        internal_rhs,
        &internal_dew_points);
    require(internal_temps.size() == 2 && internal_rhs.size() == 2 &&
                internal_dew_points.size() == 2,
            "DDM temperature, RH, and dew-point outputs must stay sample-aligned");
    for (std::size_t i = 0; i < internal_temps.size(); ++i) {
        require(close_to(
                    internal_rhs[i],
                    calculate_relative_humidity_from_temperature_dew_point(
                        internal_temps[i],
                        internal_dew_points[i]),
                    1e-12),
                "DDM RH must use the exported unrounded internal dew point");
    }

    // Source compatibility: callers that do not request dew point retain the
    // original six-argument API via the defaulted optional output.
    calculate_internal_conditions_ddm(
        {20.0},
        {50.0},
        {0.0},
        10000.0,
        internal_temps,
        internal_rhs);
    require(internal_temps.size() == 1 && internal_rhs.size() == 1,
            "DDM optional dew-point output must remain optional");

    internal_temps = {1.0};
    internal_rhs = {1.0};
    internal_dew_points = {1.0};
    calculate_internal_conditions_ddm(
        {20.0},
        {50.0, 60.0},
        {0.0},
        10000.0,
        internal_temps,
        internal_rhs,
        &internal_dew_points);
    require(internal_temps.empty() && internal_rhs.empty() &&
                internal_dew_points.empty(),
            "length mismatch must clear every DDM output");

    return 0;
}
