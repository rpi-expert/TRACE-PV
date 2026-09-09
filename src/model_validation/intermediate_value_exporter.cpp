#include "model_validation/intermediate_value_exporter.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {

std::string trim_copy(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() &&
           (value[first] == ' ' || value[first] == '\t' ||
            value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first &&
           (value[last - 1] == ' ' || value[last - 1] == '\t' ||
            value[last - 1] == '\r' || value[last - 1] == '\n')) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string ascii_lower_copy(const std::string& value) {
    std::string lower = value;
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return lower;
}

std::string valid_selector_description() {
    return "expected 'none', 'all', or a comma-separated list of zero-based "
           "case indices (for example '0,2,5')";
}

void ensure_parent_directory(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::invalid_argument("CSV output path must not be empty");
    }

    const std::filesystem::path parent = path.parent_path();
    if (parent.empty()) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        throw std::runtime_error(
            "Cannot create CSV output directory '" + parent.string() +
            "': " + error.message());
    }
}

std::ofstream open_csv(const std::filesystem::path& path) {
    ensure_parent_directory(path);
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("Cannot open CSV output file '" +
                                 path.string() + "'");
    }
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    return output;
}

void verify_csv_write(const std::ofstream& output,
                      const std::filesystem::path& path) {
    if (!output) {
        throw std::runtime_error("Failed while writing CSV output file '" +
                                 path.string() + "'");
    }
}

std::string escape_csv_string(std::string_view value) {
    const bool needs_quotes =
        value.find_first_of(",\"\r\n") != std::string_view::npos;
    if (!needs_quotes) {
        return std::string(value);
    }

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char ch : value) {
        if (ch == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

void write_numeric_cell(std::ostream& output, double value) {
    if (std::isfinite(value)) {
        output << value;
    }
}

void write_available_numeric_cell(std::ostream& output,
                                  double value,
                                  bool available) {
    if (available) {
        write_numeric_cell(output, value);
    }
}

std::string record_range_description(std::size_t begin_index,
                                     std::size_t end_index) {
    std::ostringstream text;
    text << '[' << begin_index << ',' << end_index << ')';
    return text.str();
}

} // namespace

WaveformCaseSelector::WaveformCaseSelector(
    WaveformSelectionMode mode,
    std::size_t case_count,
    std::vector<std::size_t> explicit_indices)
    : mode_(mode),
      case_count_(case_count),
      explicit_indices_(std::move(explicit_indices)) {}

WaveformCaseSelector WaveformCaseSelector::parse(std::string_view selector,
                                                 std::size_t case_count) {
    const std::string expression = trim_copy(selector);
    if (expression.empty()) {
        throw std::invalid_argument("Waveform case selector is empty; " +
                                    valid_selector_description());
    }

    const std::string keyword = ascii_lower_copy(expression);
    if (keyword == "none") {
        return WaveformCaseSelector(WaveformSelectionMode::None,
                                    case_count,
                                    {});
    }
    if (keyword == "all") {
        return WaveformCaseSelector(WaveformSelectionMode::All,
                                    case_count,
                                    {});
    }

    std::vector<std::size_t> indices;
    std::size_t token_begin = 0;
    std::size_t token_number = 0;
    while (token_begin <= expression.size()) {
        const std::size_t comma = expression.find(',', token_begin);
        const std::size_t token_end =
            comma == std::string::npos ? expression.size() : comma;
        const std::string token = trim_copy(
            std::string_view(expression).substr(token_begin,
                                                token_end - token_begin));
        ++token_number;
        if (token.empty()) {
            throw std::invalid_argument(
                "Waveform case selector '" + expression +
                "' contains an empty item at position " +
                std::to_string(token_number) + "; " +
                valid_selector_description());
        }

        std::size_t index = 0;
        const char* first = token.data();
        const char* last = token.data() + token.size();
        const std::from_chars_result parsed =
            std::from_chars(first, last, index, 10);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            throw std::invalid_argument(
                "Waveform case selector item '" + token +
                "' is not a non-negative integer; " +
                valid_selector_description());
        }
        if (index >= case_count) {
            std::ostringstream message;
            message << "Waveform case selector index " << index
                    << " is out of range for " << case_count << " cases";
            if (case_count == 0) {
                message << " (no case indices are valid)";
            } else {
                message << " (valid zero-based range is [0,"
                        << (case_count - 1) << "])";
            }
            throw std::out_of_range(message.str());
        }
        indices.push_back(index);

        if (comma == std::string::npos) {
            break;
        }
        token_begin = comma + 1;
    }

    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    return WaveformCaseSelector(WaveformSelectionMode::Explicit,
                                case_count,
                                std::move(indices));
}

bool WaveformCaseSelector::contains(std::size_t case_index) const noexcept {
    if (case_index >= case_count_) {
        return false;
    }
    if (mode_ == WaveformSelectionMode::All) {
        return true;
    }
    if (mode_ == WaveformSelectionMode::None) {
        return false;
    }
    return std::binary_search(explicit_indices_.begin(),
                              explicit_indices_.end(),
                              case_index);
}

WaveformSelectionMode WaveformCaseSelector::mode() const noexcept {
    return mode_;
}

std::size_t WaveformCaseSelector::case_count() const noexcept {
    return case_count_;
}

const std::vector<std::size_t>&
WaveformCaseSelector::explicit_indices() const noexcept {
    return explicit_indices_;
}

void write_a2s_core_matrix_csv(
    const std::filesystem::path& path,
    const std::vector<double>& time_s,
    const std::vector<double>& peak_voltage_v,
    const std::vector<double>& peak_current_a) {
    if (time_s.size() != peak_voltage_v.size() ||
        time_s.size() != peak_current_a.size()) {
        std::ostringstream message;
        message << "Cannot write A2S core matrix CSV '" << path.string()
                << "': vector lengths differ (time=" << time_s.size()
                << ", peak_voltage=" << peak_voltage_v.size()
                << ", peak_current=" << peak_current_a.size() << ')';
        throw std::invalid_argument(message.str());
    }

    std::ofstream output = open_csv(path);
    output << "time_s,peak_voltage_v,peak_current_a\n";
    for (std::size_t index = 0; index < time_s.size(); ++index) {
        write_numeric_cell(output, time_s[index]);
        output << ',';
        write_numeric_cell(output, peak_voltage_v[index]);
        output << ',';
        write_numeric_cell(output, peak_current_a[index]);
        output << '\n';
    }
    output.flush();
    verify_csv_write(output, path);
}
void write_model_validation_summary_csv(
    const std::filesystem::path& path,
    const std::vector<SimulationCase>& cases,
    const std::vector<ModelValidationRecord>& records,
    std::size_t begin_index,
    std::size_t end_index) {
    if (begin_index > end_index) {
        throw std::invalid_argument(
            "Invalid model-validation case range " +
            record_range_description(begin_index, end_index) +
            ": begin_index must not exceed end_index");
    }
    if (end_index > cases.size()) {
        std::ostringstream message;
        message << "Invalid model-validation case range "
                << record_range_description(begin_index, end_index)
                << " for " << cases.size() << " simulation cases";
        throw std::out_of_range(message.str());
    }

    std::vector<const ModelValidationRecord*> selected_records;
    selected_records.reserve(end_index - begin_index);
    for (const ModelValidationRecord& record : records) {
        if (record.case_index == ModelValidationRecord::unassigned_case_index()) {
            continue;
        }
        if (record.case_index >= cases.size()) {
            std::ostringstream message;
            message << "Model-validation record case_index "
                    << record.case_index << " is out of range for "
                    << cases.size() << " simulation cases";
            throw std::out_of_range(message.str());
        }
        if (record.case_index >= begin_index &&
            record.case_index < end_index) {
            if (record.case_processed &&
                record.dq_count > record.x_dq_ss.size()) {
                std::ostringstream message;
                message << "Model-validation record for case "
                        << record.case_index << " has dq_count="
                        << record.dq_count << ", but only "
                        << record.x_dq_ss.size()
                        << " x_dq_ss slots are available";
                throw std::invalid_argument(message.str());
            }
            selected_records.push_back(&record);
        }
    }

    std::sort(selected_records.begin(), selected_records.end(),
              [](const ModelValidationRecord* left,
                 const ModelValidationRecord* right) {
                  return left->case_index < right->case_index;
              });

    for (std::size_t index = 1; index < selected_records.size(); ++index) {
        if (selected_records[index - 1]->case_index ==
            selected_records[index]->case_index) {
            throw std::invalid_argument(
                "Duplicate model-validation record for case_index " +
                std::to_string(selected_records[index]->case_index));
        }
    }

    const std::size_t expected_count = end_index - begin_index;
    if (selected_records.size() != expected_count) {
        std::ostringstream message;
        message << "Model-validation range "
                << record_range_description(begin_index, end_index)
                << " requires exactly " << expected_count
                << " records, but " << selected_records.size()
                << " matching records were supplied";
        throw std::invalid_argument(message.str());
    }
    for (std::size_t offset = 0; offset < selected_records.size(); ++offset) {
        const std::size_t expected_index = begin_index + offset;
        if (selected_records[offset]->case_index != expected_index) {
            throw std::invalid_argument(
                "Missing model-validation record for case_index " +
                std::to_string(expected_index) + " in requested range " +
                record_range_description(begin_index, end_index));
        }
    }

    std::ofstream output = open_csv(path);
    output
        << "case_index,time,case_processed,"
           "average_model_duty_d_unitless,average_model_duty_q_unitless,dq_count,"
           "x_dq_ss_0_i1d_a,x_dq_ss_1_i1q_a,"
           "x_dq_ss_2_i2d_a,x_dq_ss_3_i2q_a,"
           "x_dq_ss_4_vcd_v,x_dq_ss_5_vcq_v,"
           "x_dq_ss_6_ilboost_a,x_dq_ss_7_vdc_v,"
           "capacitor_rms_current_a,capacitor_loss_w,"
           "inverter_average_total_loss_w,inverter_average_total_loss_valid,"
           "capacitor_surface_temperature_c,capacitor_hotspot_temperature_c,"
           "igbt_junction_temperature_c,"
           "capacitor_reference_loss_used,"
           "capacitor_reference_temperature_used,"
           "capacitor_temperature_adjusted_after_degradation,"
           "igbt_reference_loss_used,igbt_reference_temperature_used,"
           "igbt_temperature_adjusted_after_degradation,"
           "ambient_temperature_c,ambient_temperature_available,"
           "ambient_relative_humidity_percent,ambient_relative_humidity_available,"
           "internal_temperature_predicted_c,internal_temperature_predicted_available,"
           "internal_temperature_used_c,internal_temperature_used_available,"
           "internal_temperature_source,"
           "internal_relative_humidity_predicted_percent,"
           "internal_relative_humidity_predicted_available,"
           "internal_relative_humidity_used_percent,"
           "internal_relative_humidity_used_available,"
           "internal_relative_humidity_source\n";

    for (const ModelValidationRecord* record_pointer : selected_records) {
        const ModelValidationRecord& record = *record_pointer;
        const ModelValidationRecord empty_model_outputs;
        const ModelValidationRecord& values =
            record.case_processed ? record : empty_model_outputs;
        const SimulationCase& simulation_case = cases[record.case_index];

        output << record.case_index << ','
               << escape_csv_string(simulation_case.time) << ','
               << (record.case_processed ? 1 : 0) << ',';
        write_numeric_cell(output, values.average_model_duty_d);
        output << ',';
        write_numeric_cell(output, values.average_model_duty_q);
        output << ',' << values.dq_count;

        for (std::size_t dq_index = 0;
             dq_index < values.x_dq_ss.size();
             ++dq_index) {
            output << ',';
            if (dq_index < values.dq_count) {
                write_numeric_cell(output, values.x_dq_ss[dq_index]);
            }
        }

        output << ',';
        write_numeric_cell(output, values.capacitor_rms_current_a);
        output << ',';
        write_numeric_cell(output, values.capacitor_loss_w);
        output << ',';
        write_available_numeric_cell(
            output,
            values.inverter_average_total_loss_w,
            values.inverter_average_total_loss_valid);
        output << ',' << (values.inverter_average_total_loss_valid ? 1 : 0)
               << ',';
        write_numeric_cell(output, values.capacitor_surface_temperature_c);
        output << ',';
        write_numeric_cell(output, values.capacitor_hotspot_temperature_c);
        output << ',';
        write_numeric_cell(output, values.igbt_junction_temperature_c);
        output << ',' << (values.capacitor_reference_loss_used ? 1 : 0)
               << ',' << (values.capacitor_reference_temperature_used ? 1 : 0)
               << ','
               << (values.capacitor_temperature_adjusted_after_degradation ? 1 : 0)
               << ',' << (values.igbt_reference_loss_used ? 1 : 0)
               << ',' << (values.igbt_reference_temperature_used ? 1 : 0)
               << ','
               << (values.igbt_temperature_adjusted_after_degradation ? 1 : 0)
               << ',';

        write_available_numeric_cell(output,
                                     record.ambient_temperature_c,
                                     record.ambient_temperature_available);
        output << ',' << (record.ambient_temperature_available ? 1 : 0)
               << ',';
        write_available_numeric_cell(
            output,
            record.ambient_relative_humidity_percent,
            record.ambient_relative_humidity_available);
        output << ','
               << (record.ambient_relative_humidity_available ? 1 : 0)
               << ',';

        write_available_numeric_cell(
            output,
            record.internal_temperature_predicted_c,
            record.internal_temperature_predicted_available);
        output << ','
               << (record.internal_temperature_predicted_available ? 1 : 0)
               << ',';
        write_available_numeric_cell(
            output,
            record.internal_temperature_used_c,
            record.internal_temperature_used_available);
        output << ','
               << (record.internal_temperature_used_available ? 1 : 0)
               << ',' << escape_csv_string(record.internal_temperature_source)
               << ',';

        write_available_numeric_cell(
            output,
            record.internal_relative_humidity_predicted_percent,
            record.internal_relative_humidity_predicted_available);
        output << ','
               << (record.internal_relative_humidity_predicted_available ? 1 : 0)
               << ',';
        write_available_numeric_cell(
            output,
            record.internal_relative_humidity_used_percent,
            record.internal_relative_humidity_used_available);
        output << ','
               << (record.internal_relative_humidity_used_available ? 1 : 0)
               << ','
               << escape_csv_string(record.internal_relative_humidity_source)
               << '\n';
    }

    output.flush();
    verify_csv_write(output, path);
}
