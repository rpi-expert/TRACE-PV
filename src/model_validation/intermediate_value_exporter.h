#pragma once

#include "simulation_case.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// One row of scalar intermediate values for model validation.  Numeric fields
// use quiet NaN as their missing-value representation so that an unavailable
// value cannot be mistaken for a physical zero.
struct ModelValidationRecord {
    static double missing_numeric_value() noexcept {
        return std::numeric_limits<double>::quiet_NaN();
    }

    static constexpr std::size_t unassigned_case_index() noexcept {
        return std::numeric_limits<std::size_t>::max();
    }

    std::size_t case_index = unassigned_case_index();
    bool case_processed = false;

    double average_model_duty_d = missing_numeric_value();
    double average_model_duty_q = missing_numeric_value();
    std::array<double, 8> x_dq_ss{{
        missing_numeric_value(), missing_numeric_value(),
        missing_numeric_value(), missing_numeric_value(),
        missing_numeric_value(), missing_numeric_value(),
        missing_numeric_value(), missing_numeric_value()}};
    std::size_t dq_count = 0;

    double capacitor_rms_current_a = missing_numeric_value();
    double capacitor_loss_w = missing_numeric_value();
    double inverter_average_total_loss_w = missing_numeric_value();
    bool inverter_average_total_loss_valid = false;

    double capacitor_surface_temperature_c = missing_numeric_value();
    double capacitor_hotspot_temperature_c = missing_numeric_value();
    double igbt_junction_temperature_c = missing_numeric_value();

    bool capacitor_reference_loss_used = false;
    bool capacitor_reference_temperature_used = false;
    bool capacitor_temperature_adjusted_after_degradation = false;
    bool igbt_reference_loss_used = false;
    bool igbt_reference_temperature_used = false;
    bool igbt_temperature_adjusted_after_degradation = false;

    double ambient_temperature_c = missing_numeric_value();
    bool ambient_temperature_available = false;
    double ambient_relative_humidity_percent = missing_numeric_value();
    bool ambient_relative_humidity_available = false;
    double internal_temperature_predicted_c = missing_numeric_value();
    bool internal_temperature_predicted_available = false;
    double internal_temperature_used_c = missing_numeric_value();
    bool internal_temperature_used_available = false;
    std::string internal_temperature_source;  // "predicted" or "provided"

    double internal_relative_humidity_predicted_percent = missing_numeric_value();
    bool internal_relative_humidity_predicted_available = false;
    double internal_relative_humidity_used_percent = missing_numeric_value();
    bool internal_relative_humidity_used_available = false;
    // "predicted" or "recomputed_from_internal_dew_point"
    std::string internal_relative_humidity_source;
};

enum class WaveformSelectionMode {
    None,
    All,
    Explicit
};

// Parsed once and then exposed through const-only operations.  There is no
// implicit default selector: the caller is responsible for choosing one (the
// simulator's intended default is the explicit selector "0").
class WaveformCaseSelector final {
public:
    static WaveformCaseSelector parse(std::string_view selector,
                                      std::size_t case_count);

    WaveformCaseSelector(const WaveformCaseSelector&) = default;
    WaveformCaseSelector(WaveformCaseSelector&&) noexcept = default;
    WaveformCaseSelector& operator=(const WaveformCaseSelector&) = delete;
    WaveformCaseSelector& operator=(WaveformCaseSelector&&) = delete;

    bool contains(std::size_t case_index) const noexcept;
    WaveformSelectionMode mode() const noexcept;
    std::size_t case_count() const noexcept;
    const std::vector<std::size_t>& explicit_indices() const noexcept;

private:
    WaveformCaseSelector(WaveformSelectionMode mode,
                         std::size_t case_count,
                         std::vector<std::size_t> explicit_indices);

    const WaveformSelectionMode mode_;
    const std::size_t case_count_;
    const std::vector<std::size_t> explicit_indices_;
};

// Writes the per-sample core A2S validation matrix.  The three input vectors
// must have exactly the same length; their existing sample order is preserved.
void write_a2s_core_matrix_csv(
    const std::filesystem::path& path,
    const std::vector<double>& time_s,
    const std::vector<double>& peak_voltage_v,
    const std::vector<double>& peak_current_a);

// Writes one row for every case in the half-open range [begin_index,
// end_index).  Records may be supplied in any order, but must contain exactly
// one record for each case in the requested range.
void write_model_validation_summary_csv(
    const std::filesystem::path& path,
    const std::vector<SimulationCase>& cases,
    const std::vector<ModelValidationRecord>& records,
    std::size_t begin_index,
    std::size_t end_index);
