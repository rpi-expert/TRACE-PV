#include "reporting/run_report.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace tracepv::reporting;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

bool close_to(double lhs, double rhs) {
    return std::abs(lhs - rhs) <= 1e-12 * std::max(1.0, std::abs(rhs));
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "Report output missing");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("tracepv_run_report_test_" + std::to_string(suffix));
        require(std::filesystem::create_directory(path), "Cannot create test directory");
    }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

template<class Action> void require_throws(Action action, const char* message) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    require(threw, message);
}
} // namespace

int main() {
    RunReport report;
    report.wall_time_seconds = 1.2345678901234567;
    report.accepted_case_count = 12;
    report.represented_exposure_hours = 1.0;
    report.stop_reason = "max_iterations_reached";
    report.accumulated_damage.fill(0.0001);
    auto estimates = compute_lifetime_estimates(report);
    require(estimates.size() == 8, "Expected exactly eight damage modes");
    require(estimates[fan_electrical_internal].damage_mode == "fan_electrical_internal",
            "Damage modes must retain simulator ordering");
    require(close_to(*estimates[capacitor].projected_lifetime_hours, 10000.0),
            "A short profile must scale by represented hours, not one year");
    require(close_to(*estimates[capacitor].degradation_per_year, 0.876),
            "Annual degradation must normalize actual exposure");
    require(close_to(*estimates[capacitor].projected_lifetime_years, 10000.0 / 8760.0),
            "Year conversion must use 8760 hours");
    require(close_to(*estimates[capacitor].degradation_per_year *
                         *estimates[capacitor].projected_lifetime_years, 1.0),
            "Degradation per year must equal reciprocal projected lifetime years");
    require(estimates[capacitor].right_censored && !estimates[capacitor].detected_failure &&
            estimates[capacitor].status == "right_censored_projection",
            "Capped run must not claim an observed lifetime");
    require(!estimates[capacitor].failure_detection_boundary_hours,
            "Censored mode has no detected failure boundary");

    RunReport scaled = report;
    scaled.represented_exposure_hours *= 2.0;
    scaled.accepted_case_count *= 2;
    for (auto& damage : scaled.accumulated_damage) damage *= 2.0;
    require(close_to(*compute_lifetime_estimates(scaled)[0].projected_lifetime_hours,
                     *estimates[0].projected_lifetime_hours),
            "Repeating equal exposure and damage must preserve projection");

    report.accumulated_damage[capacitor] = 1.25;
    report.first_failure_boundary_hours[capacitor] = 0.75;
    report.accumulated_damage[pcb] = 1.0;
    estimates = compute_lifetime_estimates(report);
    require(estimates[capacitor].detected_failure && !estimates[capacitor].right_censored,
            "Damage at least one must indicate detected failure");
    require(*estimates[capacitor].failure_detection_boundary_hours == 0.75 &&
            close_to(*estimates[capacitor].projected_lifetime_hours, 0.8),
            "Observed reporting boundary must remain distinct from extrapolation");
    require(*estimates[pcb].failure_detection_boundary_hours == 1.0,
            "Missing first failure boundary falls back to final reporting upper bound");

    report.accumulated_damage[0] = 0.0;
    report.accumulated_damage[1] = std::numeric_limits<double>::quiet_NaN();
    report.accumulated_damage[2] = -0.01;
    report.accumulated_damage[3] = std::numeric_limits<double>::infinity();
    estimates = compute_lifetime_estimates(report);
    require(estimates[0].status == "zero_damage" && !estimates[0].projected_lifetime_hours &&
            *estimates[0].degradation_per_year == 0.0,
            "Zero damage must not serialize an infinite lifetime");
    for (const auto index : {1, 2, 3})
        require(estimates[index].status == "invalid_damage" &&
                !estimates[index].accumulated_damage &&
                !estimates[index].projected_lifetime_years &&
                !estimates[index].right_censored,
                "Invalid damage must be explicit, null and not a survival claim");

    RunReport no_exposure;
    no_exposure.accumulated_damage.fill(0.1);
    require(compute_lifetime_estimates(no_exposure)[0].status == "no_exposure",
            "Zero exposure cannot yield a projected lifetime");
    RunReport extreme = report;
    extreme.accumulated_damage[0] = std::numeric_limits<double>::denorm_min();
    require(compute_lifetime_estimates(extreme)[0].status == "projection_out_of_range" &&
            !compute_lifetime_estimates(extreme)[0].projected_lifetime_hours,
            "Overflowing extrapolation must not produce an infinite JSON number");
    no_exposure.represented_exposure_hours = -1.0;
    require_throws([&] { compute_lifetime_estimates(no_exposure); },
                   "Negative exposure must throw");
    no_exposure = report;
    no_exposure.first_failure_boundary_hours[0] = 2.0;
    require_throws([&] { compute_lifetime_estimates(no_exposure); },
                   "Failure boundary outside accepted exposure must throw");

    TemporaryDirectory tmp;
    const auto destination = tmp.path / "nested folder" / "exports";
    report.stop_reason = "capped, \"quoted\"\\path\nnext\tline";
    write_wall_time_report(destination, report);
    require(!std::filesystem::exists(destination / "lifetime.json") &&
            !std::filesystem::exists(destination / "lifetime.csv"),
            "Wall-time export must not implicitly write lifetime reports");
    write_lifetime_reports(destination, report);
    const auto lifetime_only = tmp.path / "lifetime only";
    write_lifetime_reports(lifetime_only, report);
    require(!std::filesystem::exists(lifetime_only / "wall_time.json"),
            "Lifetime export must not implicitly write a wall-time report");
    const auto wall = read_text(destination / "wall_time.json");
    const auto json = read_text(destination / "lifetime.json");
    const auto csv = read_text(destination / "lifetime.csv");
    require(wall.find("1.2345678901234567") != std::string::npos,
            "Double serialization must preserve round-trip precision");
    require(wall.find("main_entry_to_before_final_report_exports_excludes_process_shutdown") !=
                std::string::npos, "Wall time scope must be explicit");
    require(json.find("\\\"quoted\\\"\\\\path\\nnext\\tline") != std::string::npos,
            "JSON strings must escape quotes, slashes, tabs and newlines");
    require(csv.find("\"capped, \"\"quoted\"\"\\path\nnext\tline\"") != std::string::npos,
            "CSV must quote cells containing commas, quotes and newlines");
    require(json.find("\"accumulated_damage\": null") != std::string::npos,
            "Invalid damage must be JSON null");
    require(csv.find("fan_electrical_internal,invalid_damage,,1,12,5,,,,false,false,,") !=
                std::string::npos, "Invalid numeric CSV fields must remain blank");
    require(json.find("constant_average_damage_extrapolation") != std::string::npos &&
            json.find("reporting_boundary_upper_bound_not_exact_failure_time") != std::string::npos,
            "Projection and failure-boundary caveats must be machine readable");
    require(json.find("\"accepted_case_count\": 12") != std::string::npos,
            "Accepted interval count must be auditable");

    require_throws([&] { write_wall_time_report(destination / "wall_time.json", report); },
                   "A file used as output directory must fail loudly");
    const auto obstructed = tmp.path / "blocked";
    std::filesystem::create_directories(obstructed / "lifetime.json");
    require_throws([&] { write_lifetime_reports(obstructed, report); },
                   "Unwritable output target must fail loudly");
    require_throws([&] { write_wall_time_report({}, report); },
                   "Empty output directory must not silently use cwd");
    RunReport invalid_time = report;
    invalid_time.wall_time_seconds = std::numeric_limits<double>::infinity();
    require_throws([&] { write_wall_time_report(destination, invalid_time); },
                   "Non-finite wall time must fail rather than emit invalid JSON");
    return 0;
}
