#include "reporting/run_report.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace tracepv::reporting {
namespace {

constexpr const char* kModeNames[kDamageModeCount] = {
    "fan_electrical_external", "fan_electrical_internal",
    "fan_mechanical_external", "fan_mechanical_internal", "capacitor",
    "igbt_deltaT", "igbt_arrhenius", "pcb"};
constexpr const char* kProjectionMethod =
    "constant_average_damage_extrapolation";
constexpr const char* kTimingScope =
    "main_entry_to_before_final_report_exports_excludes_process_shutdown";

void validate_metadata(const RunReport& report) {
    if (!std::isfinite(report.wall_time_seconds) || report.wall_time_seconds < 0.0)
        throw std::invalid_argument("wall_time_seconds must be finite and nonnegative");
    if (!std::isfinite(report.represented_exposure_hours) ||
        report.represented_exposure_hours < 0.0)
        throw std::invalid_argument("represented_exposure_hours must be finite and nonnegative");
    if (!std::isfinite(report.case_interval_minutes) || report.case_interval_minutes <= 0.0)
        throw std::invalid_argument("case_interval_minutes must be finite and positive");
    for (const auto& boundary : report.first_failure_boundary_hours) {
        if (boundary && (!std::isfinite(*boundary) || *boundary < 0.0 ||
                         *boundary > report.represented_exposure_hours))
            throw std::invalid_argument("failure boundary must be within represented exposure");
    }
}

std::string json_string(const std::string& value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned int>(ch) << std::dec;
            else
                out << ch;
        }
    }
    out << '"';
    return out.str();
}

std::string csv_string(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"') out += '"';
        out += ch;
    }
    return out + '"';
}

void optional_number(std::ostream& out, const std::optional<double>& value,
                     const char* absent) {
    if (value) out << *value;
    else out << absent;
}

std::ostringstream report_stream() {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    return out;
}

void write_file(const std::filesystem::path& directory, const char* name,
                const std::string& content) {
    if (directory.empty()) throw std::invalid_argument("report output directory is empty");
    std::filesystem::create_directories(directory);
    const auto path = directory / name;
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    try {
        out.open(path, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
    } catch (const std::ios_base::failure& error) {
        throw std::runtime_error("Cannot write report " + path.string() + ": " + error.what());
    }
}

void json_metadata(std::ostream& out, const RunReport& report) {
    out << "  \"schema_version\": 1,\n"
        << "  \"stop_reason\": " << json_string(report.stop_reason) << ",\n"
        << "  \"represented_exposure_hours\": " << report.represented_exposure_hours << ",\n"
        << "  \"accepted_case_count\": " << report.accepted_case_count << ",\n"
        << "  \"case_interval_minutes\": " << report.case_interval_minutes << ",\n"
        << "  \"exposure_basis\": \"accepted_case_intervals_excluding_discarded_reruns\",\n";
}

} // namespace

const char* damage_mode_name(std::size_t index) {
    if (index >= kDamageModeCount) throw std::out_of_range("Invalid damage mode index");
    return kModeNames[index];
}

std::array<LifetimeEstimate, kDamageModeCount>
compute_lifetime_estimates(const RunReport& report) {
    validate_metadata(report);
    std::array<LifetimeEstimate, kDamageModeCount> estimates;
    for (std::size_t i = 0; i < kDamageModeCount; ++i) {
        auto& row = estimates[i];
        row.damage_mode = damage_mode_name(i);
        const double damage = report.accumulated_damage[i];
        if (!std::isfinite(damage) || damage < 0.0) {
            row.status = "invalid_damage";
            row.right_censored = false; // Invalid data do not establish survival.
            continue;
        }
        row.accumulated_damage = damage;
        row.detected_failure = damage >= 1.0;
        row.right_censored = !row.detected_failure;
        if (row.detected_failure) {
            // An omitted first-boundary value still proves detection no later
            // than the last accepted reporting boundary, not exact failure.
            row.failure_detection_boundary_hours =
                report.first_failure_boundary_hours[i].value_or(
                    report.represented_exposure_hours);
        }
        if (report.represented_exposure_hours <= 0.0) {
            row.status = "no_exposure";
            row.right_censored = false;
            continue;
        }
        if (damage == 0.0) {
            row.status = "zero_damage";
            row.degradation_per_year = 0.0;
            continue;
        }
        const double years = report.represented_exposure_hours / kHoursPerYear;
        const double degradation = damage / years;
        const double lifetime_hours = report.represented_exposure_hours / damage;
        const double lifetime_years = lifetime_hours / kHoursPerYear;
        if (!std::isfinite(degradation) || !std::isfinite(lifetime_hours) ||
            !std::isfinite(lifetime_years) || degradation <= 0.0 ||
            lifetime_hours <= 0.0 || lifetime_years <= 0.0) {
            row.status = "projection_out_of_range";
            continue;
        }
        row.degradation_per_year = degradation;
        row.projected_lifetime_hours = lifetime_hours;
        row.projected_lifetime_years = lifetime_years;
        row.status = row.detected_failure ? "failure_detected_at_reporting_boundary"
                                         : "right_censored_projection";
    }
    return estimates;
}

void write_wall_time_report(const std::filesystem::path& output_directory,
                            const RunReport& report) {
    validate_metadata(report);
    auto out = report_stream();
    out << "{\n";
    json_metadata(out, report);
    out << "  \"wall_time_seconds\": " << report.wall_time_seconds << ",\n"
        << "  \"timing_scope\": " << json_string(kTimingScope) << "\n}\n";
    write_file(output_directory, "wall_time.json", out.str());
}

void write_lifetime_reports(const std::filesystem::path& output_directory,
                            const RunReport& report) {
    const auto estimates = compute_lifetime_estimates(report);
    auto json = report_stream();
    json << "{\n";
    json_metadata(json, report);
    json << "  \"hours_per_year\": " << kHoursPerYear << ",\n"
         << "  \"projection_method\": " << json_string(kProjectionMethod) << ",\n"
         << "  \"projection_caveat\": \"Extrapolates the accepted exposure average damage rate; not a completed degradation-feedback lifetime simulation.\",\n"
         << "  \"failure_time_scope\": \"reporting_boundary_upper_bound_not_exact_failure_time\",\n"
         << "  \"damage_modes\": [\n";
    auto csv = report_stream();
    csv << "damage_mode,status,accumulated_damage,represented_exposure_hours,"
           "accepted_case_count,case_interval_minutes,degradation_per_year,"
           "projected_lifetime_hours,projected_lifetime_years,detected_failure,"
           "right_censored,failure_detection_boundary_hours,stop_reason,"
           "projection_method,failure_time_scope\n";
    for (std::size_t i = 0; i < estimates.size(); ++i) {
        const auto& row = estimates[i];
        json << "    {\n      \"damage_mode\": " << json_string(row.damage_mode)
             << ",\n      \"status\": " << json_string(row.status)
             << ",\n      \"accumulated_damage\": ";
        optional_number(json, row.accumulated_damage, "null");
        json << ",\n      \"degradation_per_year\": ";
        optional_number(json, row.degradation_per_year, "null");
        json << ",\n      \"projected_lifetime_hours\": ";
        optional_number(json, row.projected_lifetime_hours, "null");
        json << ",\n      \"projected_lifetime_years\": ";
        optional_number(json, row.projected_lifetime_years, "null");
        json << ",\n      \"detected_failure\": " << (row.detected_failure ? "true" : "false")
             << ",\n      \"right_censored\": " << (row.right_censored ? "true" : "false")
             << ",\n      \"failure_detection_boundary_hours\": ";
        optional_number(json, row.failure_detection_boundary_hours, "null");
        json << "\n    }" << (i + 1 == estimates.size() ? "\n" : ",\n");

        csv << csv_string(row.damage_mode) << ',' << csv_string(row.status) << ',';
        optional_number(csv, row.accumulated_damage, "");
        csv << ',' << report.represented_exposure_hours << ','
            << report.accepted_case_count << ',' << report.case_interval_minutes << ',';
        optional_number(csv, row.degradation_per_year, "");
        csv << ',';
        optional_number(csv, row.projected_lifetime_hours, "");
        csv << ',';
        optional_number(csv, row.projected_lifetime_years, "");
        csv << ',' << (row.detected_failure ? "true" : "false") << ','
            << (row.right_censored ? "true" : "false") << ',';
        optional_number(csv, row.failure_detection_boundary_hours, "");
        csv << ',' << csv_string(report.stop_reason) << ',' << kProjectionMethod
            << ",reporting_boundary_upper_bound_not_exact_failure_time\n";
    }
    json << "  ]\n}\n";
    write_file(output_directory, "lifetime.json", json.str());
    write_file(output_directory, "lifetime.csv", csv.str());
}

} // namespace tracepv::reporting
