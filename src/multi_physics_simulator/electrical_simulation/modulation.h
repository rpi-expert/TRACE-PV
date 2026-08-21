#pragma once

#include <array>
#include <vector>

#include "simulation_params.h"

struct SvmSchedule {
    std::array<double, 6> switch_times;   // t1..t6 within a switching period
    std::array<std::array<int, 3>, 7> states; // s1..s7
    double da;
    double db;
    double dc;
};

struct SpwmSchedule {
    std::array<double, 6> switch_times;
    std::array<std::array<int, 3>, 7> states;
    double da;
    double db;
    double dc;
};

struct ThreeLevelSchedule {
    std::array<double, 6> switch_times;
    std::array<std::array<int, 3>, 7> states;
    double da;  // Average duty cycle for phase a
    double db;  // Average duty cycle for phase b
    double dc;  // Average duty cycle for phase c
};

struct BoostSchedule {
    double boost_switch_time; // time when boost turns off within period
};

SvmSchedule compute_svm_schedule(const SimulationParameters& params, double t0);
SpwmSchedule compute_spwm_schedule(const SimulationParameters& params, double t0);
BoostSchedule compute_boost_schedule(const SimulationParameters& params);
ThreeLevelSchedule compute_three_level_svm_schedule(const SimulationParameters& params, double t0);
ThreeLevelSchedule compute_three_level_spwm_schedule(const SimulationParameters& params, double t0);
