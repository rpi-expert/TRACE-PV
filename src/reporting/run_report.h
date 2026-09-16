#ifndef TRACEPV_REPORTING_RUN_REPORT_H
#define TRACEPV_REPORTING_RUN_REPORT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace tracepv::reporting {

constexpr std::size_t kDamageModeCount = 8;
constexpr double kHoursPerYear = 8760.0;

// Keep this order aligned with the simulator's accumulated-stressor array.
enum DamageMode : std::size_t {
    fan_electrical_external,
    fan_electrical_internal,
    fan_mechanical_external,
    fan_mechanical_internal,
    capacitor,
    igbt_deltaT,
    igbt_arrhenius,
    pcb
};

struct RunReport {
    double wall_time_seconds = 0.0;
    double represented_exposure_hours = 0.0;
    std::uint64_t accepted_case_count = 0;
    double case_interval_minutes = 5.0;
    std::string stop_reason;
    std::array<double, kDamageModeCount> accumulated_damage{};
    // Populate at the first accepted round/batch where a mode reaches D >= 1.
    // This is a reporting-boundary upper bound, NOT the exact failure time.
    std::array<std::optional<double>, kDamageModeCount>
        first_failure_boundary_hours{};
};

struct LifetimeEstimate {
    std::string damage_mode;
    std::string status;
    std::optional<double> accumulated_damage;
    std::optional<double> degradation_per_year;
    std::optional<double> projected_lifetime_hours;
    std::optional<double> projected_lifetime_years;
    bool detected_failure = false;
    bool right_censored = true;
    std::optional<double> failure_detection_boundary_hours;
};

const char* damage_mode_name(std::size_t index);

// Uses exposure / accumulated damage, never an assumed one-year iteration.
// Invalid/zero damage and unavailable exposure yield null projection fields.
// Throws std::invalid_argument for invalid exposure/timing/boundary metadata.
std::array<LifetimeEstimate, kDamageModeCount>
compute_lifetime_estimates(const RunReport& report);

// Create output_directory if necessary; overwrite only these named reports.
// JSON uses null and CSV blank cells for unavailable numeric values. Both
// functions throw on directory/open/write/close errors (no silent failures).
void write_wall_time_report(const std::filesystem::path& output_directory,
                            const RunReport& report);
void write_lifetime_reports(const std::filesystem::path& output_directory,
                            const RunReport& report);

} // namespace tracepv::reporting

#endif
