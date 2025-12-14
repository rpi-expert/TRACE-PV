#include "rainflow_counting.h"
#include <algorithm>
#include <cmath>
#include <deque>

std::vector<double> filter_temperature_data(
    const std::vector<double>& temperature_data,
    double threshold)
{
    if (temperature_data.empty()) {
        return temperature_data;
    }
    
    std::vector<double> filtered;
    filtered.push_back(temperature_data[0]);
    
    for (size_t i = 1; i < temperature_data.size(); ++i) {
        double diff = std::abs(temperature_data[i] - filtered.back());
        if (diff >= threshold) {
            filtered.push_back(temperature_data[i]);
        }
    }
    
    return filtered;
}

RainflowResult rainflow_counting(
    const std::vector<double>& temperature_data,
    double threshold)
{
    RainflowResult result;
    
    if (temperature_data.empty()) {
        result.Tj_max = 0.0;
        return result;
    }
    
    // Filter the temperature data while tracking original indices
    std::vector<double> filtered;
    std::vector<size_t> original_indices;  // Maps filtered index to original index
    
    filtered.push_back(temperature_data[0]);
    original_indices.push_back(0);
    
    for (size_t i = 1; i < temperature_data.size(); ++i) {
        double diff = std::abs(temperature_data[i] - filtered.back());
        if (diff >= threshold) {
            filtered.push_back(temperature_data[i]);
            original_indices.push_back(i);
        }
    }
    
    if (filtered.size() < 4) {
        // Not enough points for four-point method
        result.Tj_max = *std::max_element(temperature_data.begin(), temperature_data.end());
        return result;
    }
    
    // Find maximum temperature (Tj_max) from original data
    result.Tj_max = *std::max_element(temperature_data.begin(), temperature_data.end());
    
    // Four-point rainflow counting algorithm with original index tracking
    struct HistoryPoint {
        double value;
        size_t original_idx;  // Original index in temperature_data
    };
    std::deque<HistoryPoint> history;
    struct CycleInfo {
        double range;
        size_t begin_idx;  // Original index
        size_t end_idx;    // Original index
        bool is_full;
    };
    std::vector<CycleInfo> cycles;
    
    for (size_t i = 0; i < filtered.size(); ++i) {
        double current = filtered[i];
        size_t orig_idx = original_indices[i];
        
        // Add current point to history
        history.push_back({current, orig_idx});
        
        // Keep only last 4 points for four-point method
        if (history.size() > 4) {
            history.pop_front();
        }
        
        // Need at least 4 points to form a cycle
        if (history.size() == 4) {
            double x1 = history[0].value;
            double x2 = history[1].value;
            double x3 = history[2].value;
            double x4 = history[3].value;
            size_t orig_idx1 = history[0].original_idx;
            size_t orig_idx2 = history[1].original_idx;
            size_t orig_idx3 = history[2].original_idx;
            size_t orig_idx4 = history[3].original_idx;
            
            // Check if points 2 and 3 form a peak or valley
            // Condition: (x2 > x1 && x2 > x3) || (x2 < x1 && x2 < x3)
            bool is_peak_2 = (x2 > x1 && x2 > x3) || (x2 < x1 && x2 < x3);
            bool is_peak_3 = (x3 > x2 && x3 > x4) || (x3 < x2 && x3 < x4);
            
            if (is_peak_2 && is_peak_3) {
                // Check if this forms a complete cycle
                // For four-point method: check if range from x2 to x3 is contained
                // within the range from x1 to x4
                double range_23 = std::abs(x3 - x2);
                double range_14 = std::abs(x4 - x1);
                
                if (range_23 <= range_14) {
                    // Extract cycle - this is a full cycle
                    cycles.push_back({range_23, orig_idx2, orig_idx3, true});
                    
                    // Remove the extracted cycle points (x2 and x3)
                    history.erase(history.begin() + 1, history.begin() + 3);
                }
            }
        }
    }
    
    // Process remaining points for half-cycles
    // For remaining history, create half cycles
    if (history.size() >= 2) {
        for (size_t i = 0; i < history.size() - 1; ++i) {
            double range = std::abs(history[i+1].value - history[i].value);
            if (range > threshold) {
                cycles.push_back({range, history[i].original_idx, history[i+1].original_idx, false});
            }
        }
    }
    
    // Group cycles by range and count, but keep index information
    // For cycles with same range, we'll use the max Tj from begin/end indices
    std::vector<double> ranges;
    std::vector<double> cycle_counts;
    std::vector<size_t> begin_indices;
    std::vector<size_t> end_indices;
    std::vector<bool> is_full_cycles;
    
    for (const auto& cycle : cycles) {
        double range = cycle.range;
        
        // Find if this range already exists
        auto it = std::find(ranges.begin(), ranges.end(), range);
        if (it != ranges.end()) {
            size_t index = std::distance(ranges.begin(), it);
            cycle_counts[index] += 1.0;
            // Update indices to track the cycle with max Tj
            // We'll compare later when we have access to temperature data
            begin_indices[index] = cycle.begin_idx;
            end_indices[index] = cycle.end_idx;
            is_full_cycles[index] = cycle.is_full || is_full_cycles[index]; // If any is full, mark as full
        } else {
            ranges.push_back(range);
            cycle_counts.push_back(1.0);
            begin_indices.push_back(cycle.begin_idx);
            end_indices.push_back(cycle.end_idx);
            is_full_cycles.push_back(cycle.is_full);
        }
    }
    
    result.delta_range = ranges;
    result.delta_cycle = cycle_counts;
    result.begin_idx = begin_indices;
    result.end_idx = end_indices;
    result.is_full_cycle = is_full_cycles;
    
    return result;
}

