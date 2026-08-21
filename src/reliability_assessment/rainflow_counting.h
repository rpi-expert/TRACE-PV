#ifndef RAINFLOW_COUNTING_H
#define RAINFLOW_COUNTING_H

#include "reliability_models.h"
#include <vector>

/**
 * Filter temperature data to remove small gaps below threshold
 * 
 * @param temperature_data Input temperature data
 * @param threshold Minimum temperature difference to keep
 * @return Filtered temperature data
 */
std::vector<double> filter_temperature_data(
    const std::vector<double>& temperature_data,
    double threshold
);

/**
 * Perform rainflow counting algorithm (four-point method)
 * 
 * @param temperature_data Input temperature data (should be filtered first)
 * @param threshold Threshold for removing small gaps
 * @return RainflowResult containing delta_range, delta_cycle, and Tj_max
 */
RainflowResult rainflow_counting(
    const std::vector<double>& temperature_data,
    double threshold
);

#endif // RAINFLOW_COUNTING_H

