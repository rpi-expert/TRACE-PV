#include "modulation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {

std::array<double, 3> reference_voltage_abc(const SimulationParameters& params, double t0) {
    const double omega = 2.0 * PI * params.reference_frequency;
    const double phase_shift = params.reference_phase_shift;

    std::array<double, 3> v{};
    v[0] = params.reference_phase_magnitude * std::sin(omega * t0 + phase_shift);
    v[1] = params.reference_phase_magnitude * std::sin(omega * t0 + 4.0 * PI / 3.0 + phase_shift);
    v[2] = params.reference_phase_magnitude * std::sin(omega * t0 + 2.0 * PI / 3.0 + phase_shift);
    return v;
}

} // namespace

SvmSchedule compute_svm_schedule(const SimulationParameters& params, double t0) {
    const auto v_abc = reference_voltage_abc(params, t0);
    // For 2-level: 1-stage uses v_pv, 2-stage uses vdc_target (matching MATLAB)
    const double Vdc = (params.model_stage == 1) ? params.v_pv : params.vdc_target;
    const double T_sw = params.switching_period;

    // Clark transform to alpha-beta (stationary dq with theta=0)
    const double v_q = (2.0 / 3.0) * (v_abc[0] + std::cos(-2.0 * PI / 3.0) * v_abc[1] + std::cos(2.0 * PI / 3.0) * v_abc[2]);
    const double v_d = (2.0 / 3.0) * (std::sin(0.0) * v_abc[0] + std::sin(-2.0 * PI / 3.0) * v_abc[1] + std::sin(2.0 * PI / 3.0) * v_abc[2]);

    const double m_q = v_q / Vdc;
    const double m_d = v_d / Vdc;

    const double m_max = 1.0 / std::sqrt(3.0);
    const double mag = std::sqrt(m_q * m_q + m_d * m_d);
    const double scale = (mag <= m_max || mag == 0.0) ? 1.0 : (m_max / mag);

    const double m_q_bounded = m_q * scale;
    const double m_d_bounded = m_d * scale;

    const double angle = std::atan2(-m_d_bounded, m_q_bounded);

    int sector = 1;
    if (angle >= -PI && angle < -2.0 * PI / 3.0) {
        sector = 4;
    } else if (angle >= -2.0 * PI / 3.0 && angle < -PI / 3.0) {
        sector = 5;
    } else if (angle >= -PI / 3.0 && angle < 0.0) {
        sector = 6;
    } else if (angle >= 0.0 && angle < PI / 3.0) {
        sector = 1;
    } else if (angle >= PI / 3.0 && angle < 2.0 * PI / 3.0) {
        sector = 2;
    } else if (angle >= 2.0 * PI / 3.0 && angle <= PI) {
        sector = 3;
    }

    static constexpr double lookup[6][10] = {
        {1, 1, 0, 1, 0, 0, 1.0 / 3.0, -1.0 / std::sqrt(3.0), 2.0 / 3.0, 0.0},
        {1, 1, 0, 0, 1, 0, 1.0 / 3.0, -1.0 / std::sqrt(3.0), -1.0 / 3.0, -1.0 / std::sqrt(3.0)},
        {0, 1, 1, 0, 1, 0, -2.0 / 3.0, 0.0, -1.0 / 3.0, -1.0 / std::sqrt(3.0)},
        {0, 1, 1, 0, 0, 1, -2.0 / 3.0, 0.0, -1.0 / 3.0, 1.0 / std::sqrt(3.0)},
        {1, 0, 1, 0, 0, 1, 1.0 / 3.0, 1.0 / std::sqrt(3.0), -1.0 / 3.0, 1.0 / std::sqrt(3.0)},
        {1, 0, 1, 1, 0, 0, 1.0 / 3.0, 1.0 / std::sqrt(3.0), 2.0 / 3.0, 0.0}
    };

    const auto& row = lookup[sector - 1];
    const double m_q_beta = row[6];
    const double m_d_beta = row[7];
    const double m_q_gamma = row[8];
    const double m_d_gamma = row[9];

    const double D = m_q_beta * m_d_gamma - m_q_gamma * m_d_beta;
    const double t_beta = T_sw * (m_d_gamma * m_q_bounded - m_q_gamma * m_d_bounded) / D;
    const double t_gamma = T_sw * (-m_d_beta * m_q_bounded + m_q_beta * m_d_bounded) / D;
    const double t_alpha = T_sw - t_beta - t_gamma;

    SvmSchedule sched{};

    sched.switch_times[0] = t_alpha / 4.0;
    sched.switch_times[1] = sched.switch_times[0] + t_beta / 2.0;
    sched.switch_times[2] = sched.switch_times[1] + t_gamma / 2.0;
    sched.switch_times[3] = sched.switch_times[2] + 2.0 * sched.switch_times[0];
    sched.switch_times[4] = sched.switch_times[3] + t_gamma / 2.0;
    sched.switch_times[5] = sched.switch_times[4] + t_beta / 2.0;

    sched.states[0] = {1, 1, 1};
    sched.states[1] = {static_cast<int>(row[0]), static_cast<int>(row[1]), static_cast<int>(row[2])};
    sched.states[2] = {static_cast<int>(row[3]), static_cast<int>(row[4]), static_cast<int>(row[5])};
    sched.states[3] = {0, 0, 0};
    sched.states[4] = sched.states[2];
    sched.states[5] = sched.states[1];
    sched.states[6] = sched.states[0];

    sched.da = (t_alpha / 2.0 + row[0] * t_beta + row[3] * t_gamma) / T_sw;
    sched.db = (t_alpha / 2.0 + row[1] * t_beta + row[4] * t_gamma) / T_sw;
    sched.dc = (t_alpha / 2.0 + row[2] * t_beta + row[5] * t_gamma) / T_sw;

    // Clamp 2-level duties to [0, 1] range
    sched.da = std::max(0.0, std::min(1.0, sched.da));
    sched.db = std::max(0.0, std::min(1.0, sched.db));
    sched.dc = std::max(0.0, std::min(1.0, sched.dc));

    return sched;
}

SpwmSchedule compute_spwm_schedule(const SimulationParameters& params, double t0) {
    const auto v_abc = reference_voltage_abc(params, t0);
    // For 2-level: 1-stage uses v_pv, 2-stage uses vdc_target (matching MATLAB)
    const double Vdc = (params.model_stage == 1) ? params.v_pv : params.vdc_target;
    const double T_sw = params.switching_period;

    const double slope = 2.0 * Vdc / T_sw;
    const double t1a = (v_abc[0] + 0.5 * Vdc) / slope;
    const double t2a = (v_abc[0] - 1.5 * Vdc) / -slope;
    const double t1b = (v_abc[1] + 0.5 * Vdc) / slope;
    const double t2b = (v_abc[1] - 1.5 * Vdc) / -slope;
    const double t1c = (v_abc[2] + 0.5 * Vdc) / slope;
    const double t2c = (v_abc[2] - 1.5 * Vdc) / -slope;

    SpwmSchedule sched{};

    sched.states[0] = {1, 1, 1};
    if (t1a <= t1b && t1a <= t1c) {
        sched.states[1] = {0, 1, 1};
        sched.states[2] = (t1b <= t1c) ? std::array<int, 3>{0, 0, 1} : std::array<int, 3>{0, 1, 0};
    } else if (t1b <= t1a && t1b <= t1c) {
        sched.states[1] = {1, 0, 1};
        sched.states[2] = (t1a <= t1c) ? std::array<int, 3>{0, 0, 1} : std::array<int, 3>{1, 0, 0};
    } else {
        sched.states[1] = {1, 1, 0};
        sched.states[2] = (t1a <= t1b) ? std::array<int, 3>{0, 1, 0} : std::array<int, 3>{1, 0, 0};
    }

    sched.states[3] = {0, 0, 0};

    if (t2a <= t2b && t2a <= t2c) {
        sched.states[4] = {1, 0, 0};
        sched.states[5] = (t2b <= t2c) ? std::array<int, 3>{1, 1, 0} : std::array<int, 3>{1, 0, 1};
    } else if (t2b <= t2a && t2b <= t2c) {
        sched.states[4] = {0, 1, 0};
        sched.states[5] = (t2a <= t2c) ? std::array<int, 3>{1, 1, 0} : std::array<int, 3>{0, 1, 1};
    } else {
        sched.states[4] = {0, 0, 1};
        sched.states[5] = (t2a <= t2b) ? std::array<int, 3>{1, 0, 1} : std::array<int, 3>{0, 1, 1};
    }

    sched.states[6] = {1, 1, 1};

    std::array<double, 6> times = {t1a, t1b, t1c, t2a, t2b, t2c};
    std::sort(times.begin(), times.end());
    sched.switch_times = times;

    sched.da = (v_abc[0] + 0.5 * Vdc) / Vdc;
    sched.db = (v_abc[1] + 0.5 * Vdc) / Vdc;
    sched.dc = (v_abc[2] + 0.5 * Vdc) / Vdc;

    // Clamp 2-level duties to [0, 1] range
    sched.da = std::max(0.0, std::min(1.0, sched.da));
    sched.db = std::max(0.0, std::min(1.0, sched.db));
    sched.dc = std::max(0.0, std::min(1.0, sched.dc));

    return sched;
}

BoostSchedule compute_boost_schedule(const SimulationParameters& params) {
    BoostSchedule sched{};
    sched.boost_switch_time = params.boost_duty * params.switching_period;
    return sched;
}

namespace {

struct ThreeLevelLookupRow {
    std::array<int, 12> states{};
    double m_q_alpha;
    double m_d_alpha;
    double m_q_beta;
    double m_d_beta;
    double m_q_gamma;
    double m_d_gamma;
};

double clamp_time(double value, double period) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > period) {
        return period;
    }
    return value;
}

} // namespace

ThreeLevelSchedule compute_three_level_svm_schedule(const SimulationParameters& params, double t0) {
    const auto v_abc = reference_voltage_abc(params, t0);
    const double Vdc = (params.model_stage == 1) ? params.v_pv : params.vdc_target;
    const double T_sw = params.switching_period;

    // Clarke transform (alpha-beta) equivalents
    const double v_q = (2.0 / 3.0) * (v_abc[0] + std::cos(-2.0 * PI / 3.0) * v_abc[1] + std::cos(2.0 * PI / 3.0) * v_abc[2]);
    const double v_d = (2.0 / 3.0) * (std::sin(0.0) * v_abc[0] + std::sin(-2.0 * PI / 3.0) * v_abc[1] + std::sin(2.0 * PI / 3.0) * v_abc[2]);

    const double m_q = v_q / Vdc;
    const double m_d = v_d / Vdc;

    const double m_mag = std::hypot(m_q, m_d);
    const double m_max = 1.0 / std::sqrt(3.0);
    const double scale = (m_mag <= m_max || m_mag == 0.0) ? 1.0 : (m_max / m_mag);

    const double m_q_bounded = m_q * scale;
    const double m_d_bounded = m_d * scale;

    const double angle = std::atan2(-m_d_bounded, m_q_bounded);
    const double magnitude = std::hypot(m_q_bounded, m_d_bounded);

    double x = magnitude * std::cos(angle);
    double y = magnitude * std::sin(angle);
    int sector = 1;

    if (angle >= -PI && angle < -2.0 * PI / 3.0) {
        sector = 4;
        x = magnitude * std::cos(angle + PI);
        y = magnitude * std::sin(angle + PI);
    } else if (angle >= -2.0 * PI / 3.0 && angle < -PI / 3.0) {
        sector = 5;
        x = magnitude * std::cos(angle + 2.0 * PI / 3.0);
        y = magnitude * std::sin(angle + 2.0 * PI / 3.0);
    } else if (angle >= -PI / 3.0 && angle < 0.0) {
        sector = 6;
        x = magnitude * std::cos(angle + PI / 3.0);
        y = magnitude * std::sin(angle + PI / 3.0);
    } else if (angle >= 0.0 && angle < PI / 3.0) {
        sector = 1;
        x = magnitude * std::cos(angle);
        y = magnitude * std::sin(angle);
    } else if (angle >= PI / 3.0 && angle < 2.0 * PI / 3.0) {
        sector = 2;
        x = magnitude * std::cos(angle - PI / 3.0);
        y = magnitude * std::sin(angle - PI / 3.0);
    } else if (angle >= 2.0 * PI / 3.0 && angle <= PI) {
        sector = 3;
        x = magnitude * std::cos(angle - 2.0 * PI / 3.0);
        y = magnitude * std::sin(angle - 2.0 * PI / 3.0);
    }

    int triangle = (sector - 1) * 4 + 1;
    const double inv_sqrt3 = 1.0 / std::sqrt(3.0);
    if (y > 1.0 / std::sqrt(12.0)) {
        triangle = (sector - 1) * 4 + 4;
    } else if (y < 3.0 * x * inv_sqrt3 - inv_sqrt3) {
        triangle = (sector - 1) * 4 + 2;
    } else if (y > -3.0 * x * inv_sqrt3 + inv_sqrt3) {
        triangle = (sector - 1) * 4 + 3;
    }

    static const std::array<ThreeLevelLookupRow, 24> LOOKUP = {{
        {{0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1}, 0.0, 0.0, 1.0 / 3.0, 0.0, 1.0 / 6.0, -std::sqrt(3.0) / 6.0},
        {{1, 0, 0, 2, 0, 0, 2, 1, 0, 2, 1, 1}, 1.0 / 3.0, 0.0, 2.0 / 3.0, 0.0, 0.5, -1.0 / std::sqrt(12.0)},
        {{2, 2, 1, 2, 1, 1, 2, 1, 0, 1, 1, 0}, 1.0 / 6.0, -std::sqrt(3.0) / 6.0, 1.0 / 3.0, 0.0, 0.5, -1.0 / std::sqrt(12.0)},
        {{1, 1, 0, 2, 1, 0, 2, 2, 0, 2, 2, 1}, 1.0 / 6.0, -std::sqrt(3.0) / 6.0, 0.5, -1.0 / std::sqrt(12.0), 1.0 / 3.0, -inv_sqrt3},
        {{1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0}, 0.0, 0.0, 1.0 / 6.0, -std::sqrt(3.0) / 6.0, -1.0 / 6.0, -std::sqrt(3.0) / 6.0},
        {{2, 2, 1, 2, 2, 0, 1, 2, 0, 1, 1, 0}, 1.0 / 6.0, -std::sqrt(3.0) / 6.0, 1.0 / 3.0, -inv_sqrt3, 0.0, -inv_sqrt3},
        {{1, 2, 1, 1, 2, 0, 1, 1, 0, 0, 1, 0}, -1.0 / 6.0, -std::sqrt(3.0) / 6.0, 0.0, -inv_sqrt3, 1.0 / 6.0, -std::sqrt(3.0) / 6.0},
        {{1, 2, 1, 1, 2, 0, 0, 2, 0, 0, 1, 0}, -1.0 / 6.0, -std::sqrt(3.0) / 6.0, 0.0, -inv_sqrt3, -1.0 / 3.0, -inv_sqrt3},
        {{0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1}, 0.0, 0.0, -1.0 / 6.0, -std::sqrt(3.0) / 6.0, -1.0 / 3.0, 0.0},
        {{0, 1, 0, 0, 2, 0, 0, 2, 1, 1, 2, 1}, -1.0 / 6.0, -std::sqrt(3.0) / 6.0, -1.0 / 3.0, -inv_sqrt3, -0.5, -std::sqrt(3.0) / 6.0},
        {{1, 2, 2, 1, 2, 1, 0, 2, 1, 0, 1, 1}, -1.0 / 3.0, 0.0, -1.0 / 6.0, -std::sqrt(3.0) / 6.0, -0.5, -std::sqrt(3.0) / 6.0},
        {{0, 1, 1, 0, 2, 1, 0, 2, 2, 1, 2, 2}, -1.0 / 3.0, 0.0, -0.5, -std::sqrt(3.0) / 6.0, -2.0 / 3.0, 0.0},
        {{1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0}, 0.0, 0.0, -1.0 / 3.0, 0.0, -1.0 / 6.0, std::sqrt(3.0) / 6.0},
        {{1, 2, 2, 0, 2, 2, 0, 1, 2, 0, 1, 1}, -1.0 / 3.0, 0.0, -2.0 / 3.0, 0.0, -0.5, std::sqrt(3.0) / 6.0},
        {{0, 0, 1, 0, 1, 1, 0, 1, 2, 1, 1, 2}, -1.0 / 6.0, std::sqrt(3.0) / 6.0, -1.0 / 3.0, 0.0, -0.5, std::sqrt(3.0) / 6.0},
        {{1, 1, 2, 0, 1, 2, 0, 0, 2, 0, 0, 1}, -1.0 / 6.0, std::sqrt(3.0) / 6.0, -0.5, std::sqrt(3.0) / 6.0, -1.0 / 3.0, inv_sqrt3},
        {{0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1}, 0.0, 0.0, -1.0 / 6.0, std::sqrt(3.0) / 6.0, 1.0 / 6.0, std::sqrt(3.0) / 6.0},
        {{0, 0, 1, 0, 0, 2, 1, 0, 2, 1, 1, 2}, -1.0 / 6.0, std::sqrt(3.0) / 6.0, -1.0 / 3.0, inv_sqrt3, 0.0, inv_sqrt3},
        {{2, 1, 2, 1, 1, 2, 1, 0, 2, 1, 0, 1}, 1.0 / 6.0, std::sqrt(3.0) / 6.0, -1.0 / 6.0, std::sqrt(3.0) / 6.0, 0.0, inv_sqrt3},
        {{1, 0, 1, 1, 0, 2, 2, 0, 2, 2, 1, 2}, 1.0 / 6.0, std::sqrt(3.0) / 6.0, 0.0, inv_sqrt3, 1.0 / 3.0, inv_sqrt3},
        {{1, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0}, 0.0, 0.0, 1.0 / 6.0, std::sqrt(3.0) / 6.0, 1.0 / 3.0, 0.0},
        {{2, 1, 2, 2, 0, 2, 2, 0, 1, 1, 0, 1}, 1.0 / 6.0, std::sqrt(3.0) / 6.0, 1.0 / 3.0, inv_sqrt3, 0.5, std::sqrt(3.0) / 6.0},
        {{1, 0, 0, 1, 0, 1, 2, 0, 1, 2, 1, 1}, 1.0 / 3.0, 0.0, 1.0 / 6.0, std::sqrt(3.0) / 6.0, 0.5, std::sqrt(3.0) / 6.0},
        {{2, 1, 1, 2, 0, 1, 2, 0, 0, 1, 0, 0}, 1.0 / 3.0, 0.0, 0.5, std::sqrt(3.0) / 6.0, 2.0 / 3.0, 0.0}
    }};

    const ThreeLevelLookupRow& row = LOOKUP.at(static_cast<std::size_t>(triangle - 1));

    const double m_q_alpha = row.m_q_alpha;
    const double m_d_alpha = row.m_d_alpha;
    const double m_q_beta = row.m_q_beta;
    const double m_d_beta = row.m_d_beta;
    const double m_q_gamma = row.m_q_gamma;
    const double m_d_gamma = row.m_d_gamma;

    const double mq_ref = T_sw * m_q_bounded;
    const double md_ref = T_sw * m_d_bounded;
    const double mq_alpha_scaled = T_sw * m_q_alpha;
    const double md_alpha_scaled = T_sw * m_d_alpha;
    const double mq_beta_scaled = T_sw * m_q_beta;
    const double md_beta_scaled = T_sw * m_d_beta;
    const double mq_gamma_scaled = T_sw * m_q_gamma;
    const double md_gamma_scaled = T_sw * m_d_gamma;

    double D = (m_q_beta - m_q_alpha) * (m_d_gamma - m_d_alpha) -
               (m_d_alpha - m_d_beta) * (m_q_alpha - m_q_gamma);
    if (std::abs(D) < 1e-12) {
        D = (D >= 0.0) ? 1e-12 : -1e-12;
    }

    const double t_beta = ((mq_ref - mq_alpha_scaled) * (m_d_gamma - m_d_alpha) +
                           (md_ref - md_alpha_scaled) * (m_q_alpha - m_q_gamma)) / D;
    const double t_gamma = ((md_ref - md_alpha_scaled) * (m_q_beta - m_q_alpha) +
                            (mq_ref - mq_alpha_scaled) * (m_d_alpha - m_d_beta)) / D;
    const double t_alpha = T_sw - t_beta - t_gamma;

    ThreeLevelSchedule sched{};
    std::array<std::array<int, 3>, 7> states{};

    const bool special_triangle = ((triangle - 1) % 4) == 2;
    double t1 = t_alpha / 4.0;
    double t2 = t1 + t_beta / 2.0;
    double t3 = t2 + t_gamma / 2.0;
    double t4 = t3 + (special_triangle ? t_alpha / 2.0 : t_alpha / 2.0);
    double t5 = t4 + (special_triangle ? t_beta / 2.0 : t_gamma / 2.0);
    double t6 = t5 + (special_triangle ? t_gamma / 2.0 : t_beta / 2.0);

    sched.switch_times = {clamp_time(t1, T_sw),
                          clamp_time(t2, T_sw),
                          clamp_time(t3, T_sw),
                          clamp_time(t4, T_sw),
                          clamp_time(t5, T_sw),
                          clamp_time(t6, T_sw)};

    auto convert_state = [](int raw) -> int {
        return raw - 1; // Map {0,1,2} -> {-1,0,1}
    };

    std::array<int, 12> raw_states = row.states;

    if (special_triangle) {
        states[0] = {convert_state(raw_states[0]), convert_state(raw_states[1]), convert_state(raw_states[2])};
        states[1] = {convert_state(raw_states[3]), convert_state(raw_states[4]), convert_state(raw_states[5])};
        states[2] = {convert_state(raw_states[6]), convert_state(raw_states[7]), convert_state(raw_states[8])};
        states[3] = {convert_state(raw_states[9]), convert_state(raw_states[10]), convert_state(raw_states[11])};
    } else {
        states[0] = {convert_state(raw_states[0]), convert_state(raw_states[1]), convert_state(raw_states[2])};
        states[1] = {convert_state(raw_states[3]), convert_state(raw_states[4]), convert_state(raw_states[5])};
        states[2] = {convert_state(raw_states[6]), convert_state(raw_states[7]), convert_state(raw_states[8])};
        states[3] = {convert_state(raw_states[9]), convert_state(raw_states[10]), convert_state(raw_states[11])};
    }
    states[4] = states[2];
    states[5] = states[1];
    states[6] = states[0];

    sched.states = states;
    
    // Calculate duty cycles exactly as MATLAB does (line 190-192)
    // da = ((Lookup_Table_Row(1,1) - 1)*t_alpha/2 + (Lookup_Table_Row(1,4) - 1)*t_beta + (Lookup_Table_Row(1,7) - 1)*t_gamma + (Lookup_Table_Row(1,10) - 1)*t_alpha/2) / T_sw;
    sched.da = ((raw_states[0] - 1) * t_alpha / 2.0 + 
                (raw_states[3] - 1) * t_beta + 
                (raw_states[6] - 1) * t_gamma + 
                (raw_states[9] - 1) * t_alpha / 2.0) / T_sw;
    sched.db = ((raw_states[1] - 1) * t_alpha / 2.0 + 
                (raw_states[4] - 1) * t_beta + 
                (raw_states[7] - 1) * t_gamma + 
                (raw_states[10] - 1) * t_alpha / 2.0) / T_sw;
    sched.dc = ((raw_states[2] - 1) * t_alpha / 2.0 + 
                (raw_states[5] - 1) * t_beta + 
                (raw_states[8] - 1) * t_gamma + 
                (raw_states[11] - 1) * t_alpha / 2.0) / T_sw;
    
    return sched;
}

ThreeLevelSchedule compute_three_level_spwm_schedule(const SimulationParameters& params, double t0) {
    const auto v_abc = reference_voltage_abc(params, t0);
    const double Vdc = (params.model_stage == 1) ? params.v_pv : params.vdc_target;
    const double T_sw = params.switching_period;

    auto compute_times = [&](double v_phase, int& s_high, int& s_low, double& t1, double& t2) {
        if (v_phase >= 0.0) {
            t1 = v_phase / (Vdc / T_sw);
            t2 = (v_phase - Vdc) / (-Vdc / T_sw);
            s_high = 1;
            s_low = 0;
        } else {
            t1 = (v_phase + Vdc / 2.0) / (Vdc / T_sw);
            t2 = (v_phase - Vdc / 2.0) / (-Vdc / T_sw);
            s_high = 0;
            s_low = -1;
        }
    };

    int s1a = 0, s4a = 0;
    int s1b = 0, s4b = 0;
    int s1c = 0, s4c = 0;
    double t1a = 0.0, t2a = 0.0;
    double t1b = 0.0, t2b = 0.0;
    double t1c = 0.0, t2c = 0.0;

    compute_times(v_abc[0], s1a, s4a, t1a, t2a);
    compute_times(v_abc[1], s1b, s4b, t1b, t2b);
    compute_times(v_abc[2], s1c, s4c, t1c, t2c);

    t1a = clamp_time(t1a, T_sw);
    t2a = clamp_time(t2a, T_sw);
    t1b = clamp_time(t1b, T_sw);
    t2b = clamp_time(t2b, T_sw);
    t1c = clamp_time(t1c, T_sw);
    t2c = clamp_time(t2c, T_sw);

    std::array<int, 3> s1 = {s1a, s1b, s1c};
    std::array<int, 3> s4 = {s4a, s4b, s4c};
    std::array<int, 3> s7 = s1;

    std::array<int, 3> s2{};
    std::array<int, 3> s3{};
    if (t1a <= t1b && t1a <= t1c) {
        s2 = {s4a, s1b, s1c};
        s3 = (t1b <= t1c) ? std::array<int, 3>{s4a, s4b, s1c}
                          : std::array<int, 3>{s4a, s1b, s4c};
    } else if (t1b <= t1a && t1b <= t1c) {
        s2 = {s1a, s4b, s1c};
        s3 = (t1a <= t1c) ? std::array<int, 3>{s4a, s4b, s1c}
                          : std::array<int, 3>{s1a, s4b, s4c};
    } else {
        s2 = {s1a, s1b, s4c};
        s3 = (t1a <= t1b) ? std::array<int, 3>{s4a, s1b, s4c}
                          : std::array<int, 3>{s1a, s4b, s4c};
    }

    std::array<int, 3> s5{};
    std::array<int, 3> s6{};
    if (t2a <= t2b && t2a <= t2c) {
        s5 = {s1a, s4b, s4c};
        s6 = (t2b <= t2c) ? std::array<int, 3>{s1a, s1b, s4c}
                          : std::array<int, 3>{s1a, s4b, s1c};
    } else if (t2b <= t2a && t2b <= t2c) {
        s5 = {s4a, s1b, s4c};
        s6 = (t2a <= t2c) ? std::array<int, 3>{s1a, s1b, s4c}
                          : std::array<int, 3>{s4a, s1b, s1c};
    } else {
        s5 = {s4a, s4b, s1c};
        s6 = (t2a <= t2b) ? std::array<int, 3>{s1a, s4b, s1c}
                          : std::array<int, 3>{s4a, s1b, s1c};
    }

    std::array<double, 6> times = {t1a, t1b, t1c, t2a, t2b, t2c};
    std::sort(times.begin(), times.end());

    ThreeLevelSchedule sched{};
    sched.switch_times = times;
    sched.states = {s1, s2, s3, s4, s5, s6, s7};
    
    // Calculate duty cycles exactly as MATLAB does (line 133-135)
    // da = (v_abc(1))/(Vdc/2);
    sched.da = v_abc[0] / (Vdc / 2.0);
    sched.db = v_abc[1] / (Vdc / 2.0);
    sched.dc = v_abc[2] / (Vdc / 2.0);
    
    // Clamp to [-1, 1] range
    sched.da = std::max(-1.0, std::min(1.0, sched.da));
    sched.db = std::max(-1.0, std::min(1.0, sched.db));
    sched.dc = std::max(-1.0, std::min(1.0, sched.dc));
    
    return sched;
}
