#include "model_validation/intermediate_value_exporter.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void require_throws(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

std::vector<std::string> parse_csv_row(const std::string& row) {
    std::vector<std::string> cells;
    std::string cell;
    bool quoted = false;
    for (std::size_t index = 0; index < row.size(); ++index) {
        const char ch = row[index];
        if (quoted) {
            if (ch == '"') {
                if (index + 1 < row.size() && row[index + 1] == '"') {
                    cell.push_back('"');
                    ++index;
                } else {
                    quoted = false;
                }
            } else {
                cell.push_back(ch);
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            cells.push_back(cell);
            cell.clear();
        } else {
            cell.push_back(ch);
        }
    }
    require(!quoted, "test CSV parser found an unterminated quote");
    cells.push_back(cell);
    return cells;
}

std::size_t column_index(const std::vector<std::string>& header,
                         const std::string& name) {
    for (std::size_t index = 0; index < header.size(); ++index) {
        if (header[index] == name) {
            return index;
        }
    }
    throw std::runtime_error("missing expected CSV column: " + name);
}

} // namespace

int main() {
    ModelValidationRecord missing_record;
    require(std::isnan(missing_record.average_model_duty_d),
            "numeric record fields must default to quiet NaN");
    require(std::isnan(missing_record.x_dq_ss[7]),
            "dq record fields must default to quiet NaN");

    const WaveformCaseSelector none =
        WaveformCaseSelector::parse(" none ", 4);
    require(none.mode() == WaveformSelectionMode::None &&
                !none.contains(0),
            "none selector is incorrect");

    const WaveformCaseSelector all =
        WaveformCaseSelector::parse("ALL", 4);
    require(all.mode() == WaveformSelectionMode::All &&
                all.contains(0) && all.contains(3) && !all.contains(4),
            "all selector is incorrect");

    const WaveformCaseSelector explicit_cases =
        WaveformCaseSelector::parse(" 3, 1,3 ", 4);
    require(explicit_cases.mode() == WaveformSelectionMode::Explicit &&
                explicit_cases.explicit_indices() ==
                    std::vector<std::size_t>({1, 3}) &&
                explicit_cases.contains(1) && !explicit_cases.contains(2),
            "explicit selector should be sorted and deduplicated");

    require_throws<std::invalid_argument>(
        [] { (void)WaveformCaseSelector::parse("", 4); },
        "empty selector should fail");
    require_throws<std::invalid_argument>(
        [] { (void)WaveformCaseSelector::parse("0,,1", 4); },
        "selector with an empty item should fail");
    require_throws<std::invalid_argument>(
        [] { (void)WaveformCaseSelector::parse("-1", 4); },
        "negative selector item should fail");
    require_throws<std::out_of_range>(
        [] { (void)WaveformCaseSelector::parse("4", 4); },
        "out-of-range selector item should fail");

    const auto unique_suffix = std::chrono::high_resolution_clock::now()
                                   .time_since_epoch()
                                   .count();
    const std::filesystem::path test_root =
        std::filesystem::temp_directory_path() /
        ("tracepv_intermediate_exporter_test_" +
         std::to_string(unique_suffix));

    const std::filesystem::path waveform_path =
        test_root / "nested" / "a2s_core.csv";
    write_a2s_core_matrix_csv(
        waveform_path,
        {0.0, 0.5},
        {600.0, 610.0},
        {10.0, 11.0});
    {
        std::ifstream input(waveform_path);
        std::string header;
        std::string first_row;
        std::getline(input, header);
        std::getline(input, first_row);
        require(header == "time_s,peak_voltage_v,peak_current_a",
                "A2S matrix header must have exactly three specified columns");
        require(first_row == "0,600,10", "unexpected A2S matrix first row");
    }
    require_throws<std::invalid_argument>(
        [&] {
            write_a2s_core_matrix_csv(test_root / "invalid.csv",
                                      {0.0},
                                      {1.0, 2.0},
                                      {3.0});
        },
        "unequal A2S matrix vector lengths should fail");

    std::vector<SimulationCase> cases(3);
    cases[0].time = "case, \"zero\"";
    cases[1].time = "case one";
    cases[2].time = "early-stop case";

    ModelValidationRecord case_zero;
    case_zero.case_index = 0;
    case_zero.case_processed = true;
    case_zero.average_model_duty_d = 0.1;
    case_zero.average_model_duty_q = 0.2;
    case_zero.dq_count = 6;
    for (std::size_t index = 0; index < case_zero.x_dq_ss.size(); ++index) {
        case_zero.x_dq_ss[index] = static_cast<double>(index + 1);
    }
    case_zero.ambient_temperature_c = 25.0;
    case_zero.ambient_temperature_available = true;
    case_zero.internal_temperature_predicted_c = 30.0;
    case_zero.internal_temperature_predicted_available = true;
    case_zero.internal_temperature_used_c = 31.0;
    case_zero.internal_temperature_used_available = true;
    case_zero.internal_temperature_source = "provided, sensor \"A\"";
    case_zero.internal_relative_humidity_predicted_percent = 45.0;
    case_zero.internal_relative_humidity_predicted_available = true;
    case_zero.internal_relative_humidity_used_percent = 45.0;
    case_zero.internal_relative_humidity_used_available = true;
    case_zero.internal_relative_humidity_source = "predicted";

    ModelValidationRecord case_one;
    case_one.case_index = 1;
    case_one.case_processed = true;
    case_one.dq_count = 8;
    for (std::size_t index = 0; index < case_one.x_dq_ss.size(); ++index) {
        case_one.x_dq_ss[index] = static_cast<double>(index + 11);
    }
    case_one.inverter_average_total_loss_w = 123.5;
    case_one.inverter_average_total_loss_valid = true;
    case_one.igbt_reference_loss_used = true;
    case_one.igbt_temperature_adjusted_after_degradation = true;

    ModelValidationRecord unprocessed_case;
    unprocessed_case.case_index = 2;
    unprocessed_case.average_model_duty_d = 999.0; // stale value must not leak
    unprocessed_case.capacitor_loss_w = 999.0;
    unprocessed_case.ambient_temperature_c = 40.0;
    unprocessed_case.ambient_temperature_available = true;

    const std::filesystem::path summary_path =
        test_root / "summary" / "model_validation.csv";
    write_model_validation_summary_csv(summary_path,
                                       cases,
                                       {case_one, unprocessed_case, case_zero},
                                       0,
                                       cases.size());

    {
        std::ifstream input(summary_path);
        std::string header_text;
        std::string row_zero_text;
        std::string row_one_text;
        std::string unprocessed_row_text;
        std::getline(input, header_text);
        std::getline(input, row_zero_text);
        std::getline(input, row_one_text);
        std::getline(input, unprocessed_row_text);

        const std::vector<std::string> header = parse_csv_row(header_text);
        const std::vector<std::string> row_zero = parse_csv_row(row_zero_text);
        const std::vector<std::string> row_one = parse_csv_row(row_one_text);
        const std::vector<std::string> unprocessed_row =
            parse_csv_row(unprocessed_row_text);
        require(row_zero.size() == header.size() &&
                    row_one.size() == header.size() &&
                    unprocessed_row.size() == header.size(),
                "summary rows must match the header width");
        require(row_zero[column_index(header, "case_index")] == "0" &&
                    row_one[column_index(header, "case_index")] == "1",
                "summary records must be sorted by case_index");
        require(row_zero[column_index(header, "case_processed")] == "1" &&
                    row_one[column_index(header, "case_processed")] == "1",
                "completed validation records must be marked as processed");
        require(row_zero[column_index(header, "time")] == cases[0].time,
                "summary string fields must round-trip through CSV escaping");
        require(row_zero[column_index(header, "x_dq_ss_6_ilboost_a")].empty() &&
                    row_zero[column_index(header, "x_dq_ss_7_vdc_v")].empty(),
                "stage-1-only dq columns must be empty");
        require(!row_one[column_index(header, "x_dq_ss_6_ilboost_a")].empty() &&
                    !row_one[column_index(header, "x_dq_ss_7_vdc_v")].empty(),
                "stage-2 dq columns must be populated");
        require(row_zero[column_index(header,
                                      "internal_temperature_source")] ==
                    case_zero.internal_temperature_source,
                "internal source string must be CSV escaped");
        require(row_one[column_index(header, "igbt_reference_loss_used")] == "1" &&
                    row_one[column_index(
                        header,
                        "igbt_temperature_adjusted_after_degradation")] == "1",
                "loss and adjusted-temperature provenance must be independent");
        require(unprocessed_row[column_index(header, "case_processed")] == "0" &&
                    unprocessed_row[column_index(
                        header,
                        "average_model_duty_d_unitless")].empty() &&
                    unprocessed_row[column_index(header, "capacitor_loss_w")].empty(),
                "unprocessed cases must not expose stale model outputs");
        require(unprocessed_row[column_index(
                    header,
                    "ambient_temperature_c")] == "40",
                "known environment inputs should remain visible for unprocessed cases");
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(test_root, cleanup_error);
    require(!cleanup_error, "failed to clean up exporter test files");
    return 0;
}
