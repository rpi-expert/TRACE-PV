// Workaround for CUDA 12.1 + GCC 13+ compatibility
// Must define these BEFORE any system headers
#define _Float32 float
#define _Float64 double
#define _Float128 long double
#define _Float32x double
#define _Float64x long double

#include "a2s_gpu.h"

#include "modulation.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <type_traits>

#include <cuda_runtime.h>

namespace {

constexpr double EPS_TIME = 1e-12;
constexpr double MIN_INDUCTANCE = 1e-9;
constexpr double MIN_CAPACITANCE = 1e-12;
constexpr double PI = 3.14159265358979323846;

constexpr int MAX_A2S_STATES = 12;
constexpr int MAX_STAGE1_INTERVALS = 7;
constexpr int MAX_STAGE2_INTERVALS = 8;

struct Stage1ScheduleGpu {
    double boundaries[MAX_STAGE1_INTERVALS + 1];
    int inverter[MAX_STAGE1_INTERVALS][3];
};

struct Stage2ScheduleGpu {
    double boundaries[MAX_STAGE2_INTERVALS + 1];
    int inverter[MAX_STAGE2_INTERVALS][3];
    int sboost[MAX_STAGE2_INTERVALS];
};

#define CUDA_CHECK(call)                                                                   \
    do {                                                                                   \
        cudaError_t err__ = (call);                                                        \
        if (err__ != cudaSuccess) {                                                        \
            throw std::runtime_error(std::string("CUDA error: ") + cudaGetErrorString(err__)); \
        }                                                                                  \
    } while (false)

// Device-side linear system solver for steady-state calculation
__device__ void solve_linear_system_device(const double* A_data,
                                           const double* b_data,
                                           double* x_data,
                                           int n) {
    // Simple Gaussian elimination (for small systems, n <= 8)
    double aug[8][9]; // Max n=8, so n+1=9
    
    // Create augmented matrix [A|b]
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            aug[i][j] = A_data[i * n + j];
        }
        aug[i][n] = b_data[i];
    }
    
    // Forward elimination
    for (int k = 0; k < n - 1; ++k) {
        // Find pivot
        int pivot_row = k;
        double max_val = fabs(aug[k][k]);
        for (int i = k + 1; i < n; ++i) {
            if (fabs(aug[i][k]) > max_val) {
                max_val = fabs(aug[i][k]);
                pivot_row = i;
            }
        }
        
        // Swap rows
        if (pivot_row != k) {
            for (int j = k; j <= n; ++j) {
                double temp = aug[k][j];
                aug[k][j] = aug[pivot_row][j];
                aug[pivot_row][j] = temp;
            }
        }
        
        // Eliminate
        for (int i = k + 1; i < n; ++i) {
            if (fabs(aug[k][k]) < 1e-15) {
                return; // Singular matrix
            }
            double factor = aug[i][k] / aug[k][k];
            for (int j = k; j <= n; ++j) {
                aug[i][j] -= factor * aug[k][j];
            }
        }
    }
    
    // Back substitution
    for (int i = n - 1; i >= 0; --i) {
        double sum = aug[i][n];
        for (int j = i + 1; j < n; ++j) {
            sum -= aug[i][j] * x_data[j];
        }
        if (fabs(aug[i][i]) < 1e-15) {
            return; // Singular matrix
        }
        x_data[i] = sum / aug[i][i];
    }
}

// Compute D_d and D_q from reference voltage magnitude and phase shift
__device__ void compute_dq_duties_device(const SimulationParameters& params,
                                         double t0,
                                         double& D_d,
                                         double& D_q) {
    const double w = 2.0 * PI * params.reference_frequency;
    const double V_inv_ref_mag = params.reference_phase_magnitude;
    const double delta = params.reference_phase_shift;
    
    const double v_DC_Actual = (params.model_stage == 1) ? params.v_pv : params.vdc_target;
    const double v_DC_effective = (params.topology_level == 3) ? (v_DC_Actual / 2.0) : v_DC_Actual;
    
    const double m_index = V_inv_ref_mag / v_DC_effective;
    
    // ZOH correction
    const double delay_angle = w * params.switching_period / 2.0;
    const double delta_eff = delta - delay_angle;
    
    D_d = m_index * cos(delta_eff);
    D_q = m_index * sin(delta_eff);
}

// Solve steady-state for two-level stage 1
__device__ void solve_two_level_stage1_steady_state_device(const SimulationParameters& params,
                                                            double D_d,
                                                            double D_q,
                                                            double* x_ss) {
    const int n = 6;
    double A[36]; // 6x6
    double B[6];
    
    const double L1 = fmax(params.L1, MIN_INDUCTANCE);
    const double RL1 = params.RL1;
    const double C = fmax(params.C, MIN_CAPACITANCE);
    const double RC = params.RC;
    const double L2 = fmax(params.L2, MIN_INDUCTANCE);
    const double RL2 = params.RL2;
    const double w = 2.0 * PI * params.vg_freq;
    
    const double v_Gd = params.vg_mag;
    const double v_Gq = 0.0;
    const double v_DC_Actual = params.v_pv;
    
    const double RC_L1 = RC / L1;
    const double RC_L2 = RC / L2;
    
    // Initialize A to zero
    for (int i = 0; i < 36; ++i) A[i] = 0.0;
    for (int i = 0; i < 6; ++i) B[i] = 0.0;
    
    // Build A matrix
    A[0 * 6 + 0] = -(RL1 + RC) / L1; A[0 * 6 + 1] = w;        A[0 * 6 + 2] = RC_L1;       A[0 * 6 + 4] = -1.0 / L1;
    A[1 * 6 + 0] = -w;                A[1 * 6 + 1] = -(RL1 + RC) / L1; A[1 * 6 + 3] = RC_L1;       A[1 * 6 + 5] = -1.0 / L1;
    A[2 * 6 + 0] = RC_L2;             A[2 * 6 + 2] = -(RL2 + RC) / L2; A[2 * 6 + 3] = w;           A[2 * 6 + 4] = 1.0 / L2;
    A[3 * 6 + 1] = RC_L2;             A[3 * 6 + 2] = -w;                A[3 * 6 + 3] = -(RL2 + RC) / L2; A[3 * 6 + 5] = 1.0 / L2;
    A[4 * 6 + 0] = 1.0 / C;           A[4 * 6 + 2] = -1.0 / C;          A[4 * 6 + 5] = w;
    A[5 * 6 + 1] = 1.0 / C;           A[5 * 6 + 3] = -1.0 / C;          A[5 * 6 + 4] = -w;
    
    // Build B matrix
    B[0] = (v_DC_Actual * D_d) / L1;
    B[1] = (v_DC_Actual * D_q) / L1;
    B[2] = -v_Gd / L2;
    B[3] = -v_Gq / L2;
    
    // Solve: x_ss = -A^(-1) * B
    double x[6];
    solve_linear_system_device(A, B, x, n);
    for (int i = 0; i < n; ++i) {
        x_ss[i] = -x[i];
    }
}

// Solve steady-state for two-level stage 2
__device__ void solve_two_level_stage2_steady_state_device(const SimulationParameters& params,
                                                            double dboost,
                                                            double D_d,
                                                            double D_q,
                                                            double* x_ss) {
    const int n = 8;
    double A[64]; // 8x8
    double B[8];
    
    const double L1 = fmax(params.L1, MIN_INDUCTANCE);
    const double RL1 = params.RL1;
    const double C = fmax(params.C, MIN_CAPACITANCE);
    const double RC = params.RC;
    const double L2 = fmax(params.L2, MIN_INDUCTANCE);
    const double RL2 = params.RL2;
    const double Lb = fmax(params.Lboost, MIN_INDUCTANCE);
    const double RLb = params.RLboost;
    const double CDC = fmax(params.CDC, MIN_CAPACITANCE);
    const double w = 2.0 * PI * params.vg_freq;
    
    const double v_Gd = params.vg_mag;
    const double v_Gq = 0.0;
    
    const double RC_L1 = RC / L1;
    const double RC_L2 = RC / L2;
    
    // Initialize A and B to zero
    for (int i = 0; i < 64; ++i) A[i] = 0.0;
    for (int i = 0; i < 8; ++i) B[i] = 0.0;
    
    // Build A matrix
    A[0 * 8 + 0] = -(RL1 + RC) / L1; A[0 * 8 + 1] = w;        A[0 * 8 + 2] = RC_L1; A[0 * 8 + 4] = -1.0 / L1; A[0 * 8 + 7] = D_d / L1;
    A[1 * 8 + 0] = -w;                A[1 * 8 + 1] = -(RL1 + RC) / L1; A[1 * 8 + 3] = RC_L1; A[1 * 8 + 5] = -1.0 / L1; A[1 * 8 + 7] = D_q / L1;
    A[2 * 8 + 0] = RC_L2;             A[2 * 8 + 2] = -(RL2 + RC) / L2; A[2 * 8 + 3] = w;      A[2 * 8 + 4] = 1.0 / L2;
    A[3 * 8 + 1] = RC_L2;             A[3 * 8 + 2] = -w;                A[3 * 8 + 3] = -(RL2 + RC) / L2; A[3 * 8 + 5] = 1.0 / L2;
    A[4 * 8 + 0] = 1.0 / C;           A[4 * 8 + 2] = -1.0 / C;          A[4 * 8 + 5] = w;
    A[5 * 8 + 1] = 1.0 / C;           A[5 * 8 + 3] = -1.0 / C;          A[5 * 8 + 4] = -w;
    A[6 * 8 + 6] = -RLb / Lb;         A[6 * 8 + 7] = -(1.0 - dboost) / Lb;
    A[7 * 8 + 0] = -1.5 * D_d / CDC;  A[7 * 8 + 1] = -1.5 * D_q / CDC;  A[7 * 8 + 6] = (1.0 - dboost) / CDC;
    
    // Build B matrix
    B[2] = -v_Gd / L2;
    B[3] = -v_Gq / L2;
    B[6] = params.v_pv / Lb;
    
    // Solve: x_ss = -A^(-1) * B
    double x[8];
    solve_linear_system_device(A, B, x, n);
    for (int i = 0; i < n; ++i) {
        x_ss[i] = -x[i];
    }
}

// Solve steady-state for three-level stage 1
__device__ void solve_three_level_stage1_steady_state_device(const SimulationParameters& params,
                                                              double D_d,
                                                              double D_q,
                                                              double* x_ss) {
    const int n = 6;
    double A[36];
    double B[6];
    
    const double L1 = fmax(params.L1, MIN_INDUCTANCE);
    const double RL1 = params.RL1;
    const double C = fmax(params.C, MIN_CAPACITANCE);
    const double RC = params.RC;
    const double L2 = fmax(params.L2, MIN_INDUCTANCE);
    const double RL2 = params.RL2;
    const double w = 2.0 * PI * params.vg_freq;
    
    const double v_Gd = params.vg_mag;
    const double v_Gq = 0.0;
    const double V_DC_Actual = (params.model_stage == 1) ? params.v_pv : params.vdc_target;
    
    const double RC_L1 = RC / L1;
    const double RC_L2 = RC / L2;
    
    // Initialize A and B to zero
    for (int i = 0; i < 36; ++i) A[i] = 0.0;
    for (int i = 0; i < 6; ++i) B[i] = 0.0;
    
    // Build A matrix (same as two-level stage 1)
    A[0 * 6 + 0] = -(RL1 + RC) / L1; A[0 * 6 + 1] = w;        A[0 * 6 + 2] = RC_L1;       A[0 * 6 + 4] = -1.0 / L1;
    A[1 * 6 + 0] = -w;                A[1 * 6 + 1] = -(RL1 + RC) / L1; A[1 * 6 + 3] = RC_L1;       A[1 * 6 + 5] = -1.0 / L1;
    A[2 * 6 + 0] = RC_L2;             A[2 * 6 + 2] = -(RL2 + RC) / L2; A[2 * 6 + 3] = w;           A[2 * 6 + 4] = 1.0 / L2;
    A[3 * 6 + 1] = RC_L2;             A[3 * 6 + 2] = -w;                A[3 * 6 + 3] = -(RL2 + RC) / L2; A[3 * 6 + 5] = 1.0 / L2;
    A[4 * 6 + 0] = 1.0 / C;           A[4 * 6 + 2] = -1.0 / C;          A[4 * 6 + 5] = w;
    A[5 * 6 + 1] = 1.0 / C;           A[5 * 6 + 3] = -1.0 / C;          A[5 * 6 + 4] = -w;
    
    // Build B matrix (different DC voltage for 3-level)
    B[0] = (V_DC_Actual / 2.0) * D_d / L1;
    B[1] = (V_DC_Actual / 2.0) * D_q / L1;
    B[2] = -v_Gd / L2;
    B[3] = -v_Gq / L2;
    
    // Solve: x_ss = -A^(-1) * B
    double x[6];
    solve_linear_system_device(A, B, x, n);
    for (int i = 0; i < n; ++i) {
        x_ss[i] = -x[i];
    }
}

// Solve steady-state for three-level stage 2
__device__ void solve_three_level_stage2_steady_state_device(const SimulationParameters& params,
                                                              double dboost,
                                                              double D_d,
                                                              double D_q,
                                                              double* x_ss) {
    const int n = 8;
    double A[64];
    double B[8];
    
    const double L1 = fmax(params.L1, MIN_INDUCTANCE);
    const double RL1 = params.RL1;
    const double C = fmax(params.C, MIN_CAPACITANCE);
    const double RC = params.RC;
    const double L2 = fmax(params.L2, MIN_INDUCTANCE);
    const double RL2 = params.RL2;
    const double Lb = fmax(params.Lboost, MIN_INDUCTANCE);
    const double RLb = params.RLboost;
    const double C_dc = params.CDC1;
    const double C_eq = C_dc / 2.0;
    const double w = 2.0 * PI * params.vg_freq;
    
    const double v_Gd = params.vg_mag;
    const double v_Gq = 0.0;
    
    const double RC_L1 = RC / L1;
    const double RC_L2 = RC / L2;
    
    // Initialize A and B to zero
    for (int i = 0; i < 64; ++i) A[i] = 0.0;
    for (int i = 0; i < 8; ++i) B[i] = 0.0;
    
    // Build A matrix
    A[0 * 8 + 0] = -(RL1 + RC) / L1; A[0 * 8 + 1] = w;        A[0 * 8 + 2] = RC_L1; A[0 * 8 + 4] = -1.0 / L1; A[0 * 8 + 7] = D_d / (2.0 * L1);
    A[1 * 8 + 0] = -w;                A[1 * 8 + 1] = -(RL1 + RC) / L1; A[1 * 8 + 3] = RC_L1; A[1 * 8 + 5] = -1.0 / L1; A[1 * 8 + 7] = D_q / (2.0 * L1);
    A[2 * 8 + 0] = RC_L2;             A[2 * 8 + 2] = -(RL2 + RC) / L2; A[2 * 8 + 3] = w;      A[2 * 8 + 4] = 1.0 / L2;
    A[3 * 8 + 1] = RC_L2;             A[3 * 8 + 2] = -w;                A[3 * 8 + 3] = -(RL2 + RC) / L2; A[3 * 8 + 5] = 1.0 / L2;
    A[4 * 8 + 0] = 1.0 / C;           A[4 * 8 + 2] = -1.0 / C;          A[4 * 8 + 5] = w;
    A[5 * 8 + 1] = 1.0 / C;           A[5 * 8 + 3] = -1.0 / C;          A[5 * 8 + 4] = -w;
    A[6 * 8 + 6] = -RLb / Lb;         A[6 * 8 + 7] = -(1.0 - dboost) / Lb;
    A[7 * 8 + 0] = -(3.0 / 4.0) * D_d / C_eq; A[7 * 8 + 1] = -(3.0 / 4.0) * D_q / C_eq; A[7 * 8 + 6] = (1.0 - dboost) / C_eq;
    
    // Build B matrix
    B[2] = -v_Gd / L2;
    B[3] = -v_Gq / L2;
    B[6] = params.v_pv / Lb;
    
    // Solve: x_ss = -A^(-1) * B
    double x[8];
    solve_linear_system_device(A, B, x, n);
    for (int i = 0; i < n; ++i) {
        x_ss[i] = -x[i];
    }
}

// Convert dq to abc
__device__ void dq_to_abc_device(double d_val, double q_val, double theta,
                                 double& a, double& b, double& c) {
    a = d_val * sin(theta) + q_val * cos(theta);
    b = d_val * sin(theta - 2.0 * PI / 3.0) + q_val * cos(theta - 2.0 * PI / 3.0);
    c = d_val * sin(theta + 2.0 * PI / 3.0) + q_val * cos(theta + 2.0 * PI / 3.0);
}

// Convert dq state vector to abc state vector
__device__ void convert_dq_to_abc_device(const double* x_dq,
                                        const SimulationParameters& params,
                                        double t,
                                        double* x_abc) {
    const double w = 2.0 * PI * params.vg_freq;
    const double theta = w * t;
    
    if (params.model_stage == 1) {
        // Stage 1: [I1d, I1q, I2d, I2q, Vcd, Vcq] -> [I1_a, I1_b, I1_c, I2_a, I2_b, I2_c, Vc_a, Vc_b, Vc_c]
        double i1a, i1b, i1c, i2a, i2b, i2c, vca, vcb, vcc;
        dq_to_abc_device(x_dq[0], x_dq[1], theta, i1a, i1b, i1c);
        dq_to_abc_device(x_dq[2], x_dq[3], theta, i2a, i2b, i2c);
        dq_to_abc_device(x_dq[4], x_dq[5], theta, vca, vcb, vcc);
        
        x_abc[0] = i1a; x_abc[1] = i1b; x_abc[2] = i1c;
        x_abc[3] = i2a; x_abc[4] = i2b; x_abc[5] = i2c;
        x_abc[6] = vca; x_abc[7] = vcb; x_abc[8] = vcc;
    } else if (params.topology_level == 2) {
        // Stage 2 (2-level): [I1d, I1q, I2d, I2q, Vcd, Vcq, ILb, Vdc]
        double i1a, i1b, i1c, i2a, i2b, i2c, vca, vcb, vcc;
        dq_to_abc_device(x_dq[0], x_dq[1], theta, i1a, i1b, i1c);
        dq_to_abc_device(x_dq[2], x_dq[3], theta, i2a, i2b, i2c);
        dq_to_abc_device(x_dq[4], x_dq[5], theta, vca, vcb, vcc);
        
        x_abc[0] = i1a; x_abc[1] = i1b; x_abc[2] = i1c;
        x_abc[3] = i2a; x_abc[4] = i2b; x_abc[5] = i2c;
        x_abc[6] = vca; x_abc[7] = vcb; x_abc[8] = vcc;
        x_abc[9] = x_dq[6];  // IL_boost
        x_abc[10] = x_dq[7]; // Vdc
    } else {
        // Stage 2 (3-level): [I1d, I1q, I2d, I2q, Vcd, Vcq, ILb, Vdc]
        double i1a, i1b, i1c, i2a, i2b, i2c, vca, vcb, vcc;
        dq_to_abc_device(x_dq[0], x_dq[1], theta, i1a, i1b, i1c);
        dq_to_abc_device(x_dq[2], x_dq[3], theta, i2a, i2b, i2c);
        dq_to_abc_device(x_dq[4], x_dq[5], theta, vca, vcb, vcc);
        
        x_abc[0] = i1a; x_abc[1] = i1b; x_abc[2] = i1c;
        x_abc[3] = i2a; x_abc[4] = i2b; x_abc[5] = i2c;
        x_abc[6] = vca; x_abc[7] = vcb; x_abc[8] = vcc;
        x_abc[9] = x_dq[6];  // IL_boost
        x_abc[10] = x_dq[7] / 2.0; // Vdc1
        x_abc[11] = x_dq[7] / 2.0; // Vdc2
    }
}

// A2S derivative functions (same as v2)
__device__ void derivative_two_level_stage1(const SimulationParameters& params,
                                            double t,
                                            const double* x,
                                            double* dx,
                                            int sa,
                                            int sb,
                                            int sc) {
    for (int i = 0; i < params.num_states; ++i) {
        dx[i] = 0.0;
    }

    const double L1 = fmax(params.L1, MIN_INDUCTANCE);
    const double RL1 = params.RL1;
    const double C = fmax(params.C, MIN_CAPACITANCE);
    const double RC = params.RC;
    const double L2 = fmax(params.L2, MIN_INDUCTANCE);
    const double RL2 = params.RL2;

    const double angle_base = 2.0 * PI * params.vg_freq * t + params.vg_phase;
    const double vg_a = params.vg_mag * sin(angle_base);
    const double vg_b = params.vg_mag * sin(angle_base + 4.0 * PI / 3.0);
    const double vg_c = params.vg_mag * sin(angle_base + 2.0 * PI / 3.0);

    dx[0] = (-(RL1 + RC) / L1) * x[0] + (RC / L1) * x[3] - (1.0 / L1) * x[6] +
            (1.0 / L1) * (2.0 / 3.0 * sa - 1.0 / 3.0 * sb - 1.0 / 3.0 * sc) * params.v_pv;
    dx[1] = (-(RL1 + RC) / L1) * x[1] + (RC / L1) * x[4] - (1.0 / L1) * x[7] +
            (1.0 / L1) * (-1.0 / 3.0 * sa + 2.0 / 3.0 * sb - 1.0 / 3.0 * sc) * params.v_pv;
    dx[2] = (-(RL1 + RC) / L1) * x[2] + (RC / L1) * x[5] - (1.0 / L1) * x[8] +
            (1.0 / L1) * (-1.0 / 3.0 * sa - 1.0 / 3.0 * sb + 2.0 / 3.0 * sc) * params.v_pv;

    dx[3] = (RC / L2) * x[0] - ((RC + RL2) / L2) * x[3] + (1.0 / L2) * x[6] - vg_a / L2;
    dx[4] = (RC / L2) * x[1] - ((RC + RL2) / L2) * x[4] + (1.0 / L2) * x[7] - vg_b / L2;
    dx[5] = (RC / L2) * x[2] - ((RC + RL2) / L2) * x[5] + (1.0 / L2) * x[8] - vg_c / L2;

    dx[6] = (1.0 / C) * x[0] - (1.0 / C) * x[3];
    dx[7] = (1.0 / C) * x[1] - (1.0 / C) * x[4];
    dx[8] = (1.0 / C) * x[2] - (1.0 / C) * x[5];
}

__device__ void derivative_two_level_stage2(const SimulationParameters& params,
                                            double t,
                                            const double* x,
                                            double* dx,
                                            int sboost,
                                            int sa,
                                            int sb,
                                            int sc) {
    for (int i = 0; i < params.num_states; ++i) {
        dx[i] = 0.0;
    }

    const double L1 = fmax(params.L1, MIN_INDUCTANCE);
    const double RL1 = params.RL1;
    const double C = fmax(params.C, MIN_CAPACITANCE);
    const double RC = params.RC;
    const double L2 = fmax(params.L2, MIN_INDUCTANCE);
    const double RL2 = params.RL2;
    const double Lb = fmax(params.Lboost, MIN_INDUCTANCE);
    const double RLb = params.RLboost;
    const double CDC = fmax(params.CDC, MIN_CAPACITANCE);

    const double vdc = x[10];

    const double angle_base = 2.0 * PI * params.vg_freq * t + params.vg_phase;
    const double vg_a = params.vg_mag * sin(angle_base);
    const double vg_b = params.vg_mag * sin(angle_base + 4.0 * PI / 3.0);
    const double vg_c = params.vg_mag * sin(angle_base + 2.0 * PI / 3.0);

    dx[0] = (-(RL1 + RC) / L1) * x[0] + (RC / L1) * x[3] - (1.0 / L1) * x[6] +
            (1.0 / L1) * (2.0 / 3.0 * sa - 1.0 / 3.0 * sb - 1.0 / 3.0 * sc) * vdc;
    dx[1] = (-(RL1 + RC) / L1) * x[1] + (RC / L1) * x[4] - (1.0 / L1) * x[7] +
            (1.0 / L1) * (-1.0 / 3.0 * sa + 2.0 / 3.0 * sb - 1.0 / 3.0 * sc) * vdc;
    dx[2] = (-(RL1 + RC) / L1) * x[2] + (RC / L1) * x[5] - (1.0 / L1) * x[8] +
            (1.0 / L1) * (-1.0 / 3.0 * sa - 1.0 / 3.0 * sb + 2.0 / 3.0 * sc) * vdc;

    dx[3] = (RC / L2) * x[0] - ((RC + RL2) / L2) * x[3] + (1.0 / L2) * x[6] - vg_a / L2;
    dx[4] = (RC / L2) * x[1] - ((RC + RL2) / L2) * x[4] + (1.0 / L2) * x[7] - vg_b / L2;
    dx[5] = (RC / L2) * x[2] - ((RC + RL2) / L2) * x[5] + (1.0 / L2) * x[8] - vg_c / L2;

    dx[6] = (1.0 / C) * x[0] - (1.0 / C) * x[3];
    dx[7] = (1.0 / C) * x[1] - (1.0 / C) * x[4];
    dx[8] = (1.0 / C) * x[2] - (1.0 / C) * x[5];

    dx[9] = -(RLb / Lb) * x[9] - (1.0 - sboost) / Lb * vdc + params.v_pv / Lb;
    dx[10] = ((1.0 - sboost) * x[9] - (sa * x[0] + sb * x[1] + sc * x[2])) / CDC;
}

__device__ void derivative_three_level_stage1(const SimulationParameters& params,
                                              double t,
                                              const double* x,
                                              double* dx,
                                              int sag,
                                              int sbg,
                                              int scg) {
    for (int i = 0; i < params.num_states; ++i) {
        dx[i] = 0.0;
    }

    const double L1 = fmax(params.L1, MIN_INDUCTANCE);
    const double RL1 = params.RL1;
    const double C = fmax(params.C, MIN_CAPACITANCE);
    const double RC = params.RC;
    const double L2 = fmax(params.L2, MIN_INDUCTANCE);
    const double RL2 = params.RL2;

    const double angle_base = 2.0 * PI * params.vg_freq * t + params.vg_phase;
    const double vg_a = params.vg_mag * sin(angle_base);
    const double vg_b = params.vg_mag * sin(angle_base + 4.0 * PI / 3.0);
    const double vg_c = params.vg_mag * sin(angle_base + 2.0 * PI / 3.0);

    const double term_pos_a = (1.0 / 3.0 * sag * (sag + 1.0) -
                               1.0 / 6.0 * sbg * (sbg + 1.0) -
                               1.0 / 6.0 * scg * (scg + 1.0));
    const double term_neg_a = (-1.0 / 3.0 * (sag - 1.0) * sag +
                               1.0 / 6.0 * (sbg - 1.0) * sbg +
                               1.0 / 6.0 * (scg - 1.0) * scg);
    const double term_pos_b = (-1.0 / 6.0 * sag * (sag + 1.0) +
                               1.0 / 3.0 * sbg * (sbg + 1.0) -
                               1.0 / 6.0 * scg * (scg + 1.0));
    const double term_neg_b = (1.0 / 6.0 * (sag - 1.0) * sag -
                               1.0 / 3.0 * (sbg - 1.0) * sbg +
                               1.0 / 6.0 * (scg - 1.0) * scg);
    const double term_pos_c = (-1.0 / 6.0 * sag * (sag + 1.0) -
                               1.0 / 6.0 * sbg * (sbg + 1.0) +
                               1.0 / 3.0 * scg * (scg + 1.0));
    const double term_neg_c = (1.0 / 6.0 * (sag - 1.0) * sag +
                               1.0 / 6.0 * (sbg - 1.0) * sbg -
                               1.0 / 3.0 * (scg - 1.0) * scg);

    const double v_half = params.v_pv / 2.0;
    const double inv_a = v_half * (term_pos_a + term_neg_a);
    const double inv_b = v_half * (term_pos_b + term_neg_b);
    const double inv_c = v_half * (term_pos_c + term_neg_c);

    dx[0] = (-(RL1 + RC) / L1) * x[0] + (RC / L1) * x[3] - (1.0 / L1) * x[6] + inv_a / L1;
    dx[1] = (-(RL1 + RC) / L1) * x[1] + (RC / L1) * x[4] - (1.0 / L1) * x[7] + inv_b / L1;
    dx[2] = (-(RL1 + RC) / L1) * x[2] + (RC / L1) * x[5] - (1.0 / L1) * x[8] + inv_c / L1;

    dx[3] = (RC / L2) * x[0] - ((RC + RL2) / L2) * x[3] + (1.0 / L2) * x[6] - vg_a / L2;
    dx[4] = (RC / L2) * x[1] - ((RC + RL2) / L2) * x[4] + (1.0 / L2) * x[7] - vg_b / L2;
    dx[5] = (RC / L2) * x[2] - ((RC + RL2) / L2) * x[5] + (1.0 / L2) * x[8] - vg_c / L2;

    dx[6] = (1.0 / C) * x[0] - (1.0 / C) * x[3];
    dx[7] = (1.0 / C) * x[1] - (1.0 / C) * x[4];
    dx[8] = (1.0 / C) * x[2] - (1.0 / C) * x[5];
}

__device__ void derivative_three_level_stage2(const SimulationParameters& params,
                                              double t,
                                              const double* x,
                                              double* dx,
                                              int sboost,
                                              int sag,
                                              int sbg,
                                              int scg) {
    for (int i = 0; i < params.num_states; ++i) {
        dx[i] = 0.0;
    }

    const double L1 = fmax(params.L1, MIN_INDUCTANCE);
    const double RL1 = params.RL1;
    const double C = fmax(params.C, MIN_CAPACITANCE);
    const double RC = params.RC;
    const double L2 = fmax(params.L2, MIN_INDUCTANCE);
    const double RL2 = params.RL2;
    const double Lb = fmax(params.Lboost, MIN_INDUCTANCE);
    const double RLb = params.RLboost;
    const double CDC1 = fmax(params.CDC1, MIN_CAPACITANCE);
    const double CDC2 = fmax(params.CDC2, MIN_CAPACITANCE);

    const double Vcdc1 = x[10];
    const double Vcdc2 = x[11];

    const double angle_base = 2.0 * PI * params.vg_freq * t + params.vg_phase;
    const double vg_a = params.vg_mag * sin(angle_base);
    const double vg_b = params.vg_mag * sin(angle_base + 4.0 * PI / 3.0);
    const double vg_c = params.vg_mag * sin(angle_base + 2.0 * PI / 3.0);

    const double term_upper_a = (1.0 / 3.0 * sag * (sag + 1.0) -
                                 1.0 / 6.0 * sbg * (sbg + 1.0) -
                                 1.0 / 6.0 * scg * (scg + 1.0));
    const double term_lower_a = (-1.0 / 3.0 * (sag - 1.0) * sag +
                                 1.0 / 6.0 * (sbg - 1.0) * sbg +
                                 1.0 / 6.0 * (scg - 1.0) * scg);
    const double term_upper_b = (-1.0 / 6.0 * sag * (sag + 1.0) +
                                 1.0 / 3.0 * sbg * (sbg + 1.0) -
                                 1.0 / 6.0 * scg * (scg + 1.0));
    const double term_lower_b = (1.0 / 6.0 * (sag - 1.0) * sag -
                                 1.0 / 3.0 * (sbg - 1.0) * sbg +
                                 1.0 / 6.0 * (scg - 1.0) * scg);
    const double term_upper_c = (-1.0 / 6.0 * sag * (sag + 1.0) -
                                 1.0 / 6.0 * sbg * (sbg + 1.0) +
                                 1.0 / 3.0 * scg * (scg + 1.0));
    const double term_lower_c = (1.0 / 6.0 * (sag - 1.0) * sag +
                                 1.0 / 6.0 * (sbg - 1.0) * sbg -
                                 1.0 / 3.0 * (scg - 1.0) * scg);

    const double inv_a = (term_upper_a * Vcdc1 + term_lower_a * Vcdc2) / L1;
    const double inv_b = (term_upper_b * Vcdc1 + term_lower_b * Vcdc2) / L1;
    const double inv_c = (term_upper_c * Vcdc1 + term_lower_c * Vcdc2) / L1;

    dx[0] = (-(RL1 + RC) / L1) * x[0] + (RC / L1) * x[3] - (1.0 / L1) * x[6] + inv_a;
    dx[1] = (-(RL1 + RC) / L1) * x[1] + (RC / L1) * x[4] - (1.0 / L1) * x[7] + inv_b;
    dx[2] = (-(RL1 + RC) / L1) * x[2] + (RC / L1) * x[5] - (1.0 / L1) * x[8] + inv_c;

    dx[3] = (RC / L2) * x[0] - ((RC + RL2) / L2) * x[3] + (1.0 / L2) * x[6] - vg_a / L2;
    dx[4] = (RC / L2) * x[1] - ((RC + RL2) / L2) * x[4] + (1.0 / L2) * x[7] - vg_b / L2;
    dx[5] = (RC / L2) * x[2] - ((RC + RL2) / L2) * x[5] + (1.0 / L2) * x[8] - vg_c / L2;

    dx[6] = (1.0 / C) * x[0] - (1.0 / C) * x[3];
    dx[7] = (1.0 / C) * x[1] - (1.0 / C) * x[4];
    dx[8] = (1.0 / C) * x[2] - (1.0 / C) * x[5];

    const double vdc_total = Vcdc1 + Vcdc2;
    dx[9] = -(RLb / Lb) * x[9] - (1.0 - sboost) / Lb * vdc_total + params.v_pv / Lb;

    dx[10] = (-0.5 / CDC1) * (sag * (sag + 1.0) * x[0] +
                              sbg * (sbg + 1.0) * x[1] +
                              scg * (scg + 1.0) * x[2]) +
             (1.0 - sboost) / CDC1 * x[9];

    dx[11] = (0.5 / CDC2) * ((sag - 1.0) * sag * x[0] +
                             (sbg - 1.0) * sbg * x[1] +
                             (scg - 1.0) * scg * x[2]) +
              (1.0 - sboost) / CDC2 * x[9];
}

// Device functors for derivatives
struct TwoLevelStage1DerivativeFunctorDevice {
    SimulationParameters params;
    int sa;
    int sb;
    int sc;

    __device__ void operator()(double t, const double* x_state, double* dx_state) const {
        derivative_two_level_stage1(params, t, x_state, dx_state, sa, sb, sc);
    }
};

struct TwoLevelStage2DerivativeFunctorDevice {
    SimulationParameters params;
    int sboost;
    int sa;
    int sb;
    int sc;

    __device__ void operator()(double t, const double* x_state, double* dx_state) const {
        derivative_two_level_stage2(params, t, x_state, dx_state, sboost, sa, sb, sc);
    }
};

struct ThreeLevelStage1DerivativeFunctorDevice {
    SimulationParameters params;
    int sag;
    int sbg;
    int scg;

    __device__ void operator()(double t, const double* x_state, double* dx_state) const {
        derivative_three_level_stage1(params, t, x_state, dx_state, sag, sbg, scg);
    }
};

struct ThreeLevelStage2DerivativeFunctorDevice {
    SimulationParameters params;
    int sboost;
    int sag;
    int sbg;
    int scg;

    __device__ void operator()(double t, const double* x_state, double* dx_state) const {
        derivative_three_level_stage2(params, t, x_state, dx_state, sboost, sag, sbg, scg);
    }
};

// RK4 step
template <typename DerivativeFn>
__device__ void rk4_step_device(double* state,
                                double t,
                                double h,
                                int n,
                                DerivativeFn derivative) {
    double k1[MAX_A2S_STATES];
    double k2[MAX_A2S_STATES];
    double k3[MAX_A2S_STATES];
    double k4[MAX_A2S_STATES];
    double temp[MAX_A2S_STATES];

    derivative(t, state, k1);
    for (int i = 0; i < n; ++i) {
        temp[i] = state[i] + 0.5 * h * k1[i];
    }

    derivative(t + 0.5 * h, temp, k2);
    for (int i = 0; i < n; ++i) {
        temp[i] = state[i] + 0.5 * h * k2[i];
    }

    derivative(t + 0.5 * h, temp, k3);
    for (int i = 0; i < n; ++i) {
        temp[i] = state[i] + h * k3[i];
    }

    derivative(t + h, temp, k4);
    for (int i = 0; i < n; ++i) {
        state[i] += (h / 6.0) * (k1[i] + 2.0 * k2[i] + 2.0 * k3[i] + k4[i]);
    }
}

// Get switching states for a given time within a switching period
// t_local is relative to period_start (0 to switching_period)
// schedule.boundaries are absolute (period_start to period_start + switching_period)
// So we need to get period_start from boundaries[0]
__device__ void get_switching_states_two_level_stage1(const Stage1ScheduleGpu& schedule,
                                                      double t_local,
                                                      int& sa, int& sb, int& sc) {
    const double period_start = schedule.boundaries[0]; // First boundary is period_start
    const double t_absolute = period_start + t_local;
    
    int interval = 0;
    for (int i = 0; i < MAX_STAGE1_INTERVALS; ++i) {
        if (t_absolute >= schedule.boundaries[i] && t_absolute < schedule.boundaries[i + 1]) {
            interval = i;
            break;
        }
    }
    sa = schedule.inverter[interval][0];
    sb = schedule.inverter[interval][1];
    sc = schedule.inverter[interval][2];
}

__device__ void get_switching_states_two_level_stage2(const Stage2ScheduleGpu& schedule,
                                                      double t_local,
                                                      int& sboost, int& sa, int& sb, int& sc) {
    const double period_start = schedule.boundaries[0];
    const double t_absolute = period_start + t_local;
    
    int interval = 0;
    for (int i = 0; i < MAX_STAGE2_INTERVALS; ++i) {
        if (t_absolute >= schedule.boundaries[i] && t_absolute < schedule.boundaries[i + 1]) {
            interval = i;
            break;
        }
    }
    sboost = schedule.sboost[interval];
    sa = schedule.inverter[interval][0];
    sb = schedule.inverter[interval][1];
    sc = schedule.inverter[interval][2];
}

__device__ void get_switching_states_three_level_stage1(const Stage1ScheduleGpu& schedule,
                                                         double t_local,
                                                         int& sag, int& sbg, int& scg) {
    const double period_start = schedule.boundaries[0];
    const double t_absolute = period_start + t_local;
    
    int interval = 0;
    for (int i = 0; i < MAX_STAGE1_INTERVALS; ++i) {
        if (t_absolute >= schedule.boundaries[i] && t_absolute < schedule.boundaries[i + 1]) {
            interval = i;
            break;
        }
    }
    sag = schedule.inverter[interval][0];
    sbg = schedule.inverter[interval][1];
    scg = schedule.inverter[interval][2];
}

__device__ void get_switching_states_three_level_stage2(const Stage2ScheduleGpu& schedule,
                                                        double t_local,
                                                        int& sboost, int& sag, int& sbg, int& scg) {
    const double period_start = schedule.boundaries[0];
    const double t_absolute = period_start + t_local;
    
    int interval = 0;
    for (int i = 0; i < MAX_STAGE2_INTERVALS; ++i) {
        if (t_absolute >= schedule.boundaries[i] && t_absolute < schedule.boundaries[i + 1]) {
            interval = i;
            break;
        }
    }
    sboost = schedule.sboost[interval];
    sag = schedule.inverter[interval][0];
    sbg = schedule.inverter[interval][1];
    scg = schedule.inverter[interval][2];
}


// Unified kernel: each thread handles one timestep
__global__ void unified_kernel_two_level_stage1(SimulationParameters params,
                                                const Stage1ScheduleGpu* schedules,
                                                int num_samples,
                                                double dt,
                                                double* output_states,
                                                double* output_time_points,
                                                int* output_switching_states) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_samples) {
        return;
    }
    
    // Calculate which period and sample within period (matching v2's approach)
    const int samples_per_period = MAX_STAGE1_INTERVALS * params.a2s_points_per_interval;
    const int period = idx / samples_per_period;
    const int sample_in_period = idx % samples_per_period;
    const int interval = sample_in_period / params.a2s_points_per_interval;
    const int sample_in_interval = sample_in_period % params.a2s_points_per_interval;
    
    const double period_start = period * params.switching_period;
    const Stage1ScheduleGpu& schedule = schedules[period];
    
    // Compute time point like v2: t_start + duration * alpha
    const double t_start = schedule.boundaries[interval];
    const double t_end = schedule.boundaries[interval + 1];
    const double duration = t_end - t_start;
    const double alpha = (params.a2s_points_per_interval == 1) ? 
        0.0 : static_cast<double>(sample_in_interval) / static_cast<double>(params.a2s_points_per_interval - 1);
    const double t = t_start + duration * alpha;
    const double t_local = t - period_start;
    
    // Step 1: Calculate average model values (same as v2: compute steady-state ONCE at t=0.0)
    // v2 computes duties at t=0.0, solves steady-state once, then converts dq->abc at each time
    double D_d_0, D_q_0;
    compute_dq_duties_device(params, 0.0, D_d_0, D_q_0);
    
    double x_dq_ss[8]; // Max 8 states for dq - steady-state computed once
    solve_two_level_stage1_steady_state_device(params, D_d_0, D_q_0, x_dq_ss);
    
    // Convert steady-state dq to abc at period_start (initial condition for A2S)
    double x_abc_avg_start[MAX_A2S_STATES];
    convert_dq_to_abc_device(x_dq_ss, params, period_start, x_abc_avg_start);
    
    // Step 2: Use average model at period start as initial condition for A2S
    double x_a2s[MAX_A2S_STATES];
    for (int i = 0; i < params.num_states; ++i) {
        x_a2s[i] = x_abc_avg_start[i];
    }
    
    // Step 3: Run A2S simulation from period_start to this timestep
    // Integrate through switching intervals, respecting boundaries
    if (t > period_start + EPS_TIME) {
        double current_t = period_start;
        const double target_t = t;
        
        // Find which interval we start in (boundaries are absolute times)
        int current_interval = 0;
        for (int i = 0; i < MAX_STAGE1_INTERVALS; ++i) {
            if (schedule.boundaries[i] <= current_t && current_t < schedule.boundaries[i + 1]) {
                current_interval = i;
                break;
            }
        }
        
        // Integrate step by step through intervals until we reach target_t
        while (current_t < target_t - EPS_TIME) {
            // Get switching state from current interval (like v2 does)
            // v2 uses the switching state from the current interval for the entire step,
            // even if the step crosses a boundary
            const double current_t_local = current_t - period_start;
            int sa, sb, sc;
            get_switching_states_two_level_stage1(schedule, current_t_local, sa, sb, sc);
            TwoLevelStage1DerivativeFunctorDevice derivative{params, sa, sb, sc};
            
            const double interval_end = (schedule.boundaries[current_interval + 1] < target_t) ? 
                schedule.boundaries[current_interval + 1] : target_t;
            const double interval_duration = interval_end - current_t;
            
            if (interval_duration > EPS_TIME) {
                // Use step size matching v2's a2s_points_per_interval approach
                // v2 uses a2s_points_per_interval samples per interval for integration
                const int steps_in_interval = params.a2s_points_per_interval;
                const double h = interval_duration / steps_in_interval;
                
                for (int step = 0; step < steps_in_interval; ++step) {
                    const double t_step = current_t + step * h;
                    // Use the same switching state for all steps in this interval segment
                    rk4_step_device(x_a2s, t_step, h, params.num_states, derivative);
                }
            }
            
            // Advance to next interval or target
            current_t = interval_end;
            // Advance interval if we've reached the boundary (with tolerance for floating point)
            if (current_interval < MAX_STAGE1_INTERVALS - 1) {
                if (current_t >= schedule.boundaries[current_interval + 1] - EPS_TIME) {
                    current_interval++;
                }
            }
        }
    }
    // If t == period_start, x_a2s already contains the average model value
    
    // Store results
    output_time_points[idx] = t;
    for (int i = 0; i < params.num_states; ++i) {
        output_states[idx * params.num_states + i] = x_a2s[i];
    }
    
    int sa, sb, sc;
    get_switching_states_two_level_stage1(schedule, t_local, sa, sb, sc);
    output_switching_states[idx * 3 + 0] = sa;
    output_switching_states[idx * 3 + 1] = sb;
    output_switching_states[idx * 3 + 2] = sc;
}

// Similar kernels for other topologies (two_level_stage2, three_level_stage1, three_level_stage2)
// For brevity, I'll create simplified versions that follow the same pattern

__global__ void unified_kernel_two_level_stage2(SimulationParameters params,
                                                const Stage2ScheduleGpu* schedules,
                                                int num_samples,
                                                double dt,
                                                double* output_states,
                                                double* output_time_points,
                                                int* output_switching_states) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_samples) {
        return;
    }
    
    // Calculate which period and sample within period (matching v2's approach)
    const int samples_per_period = MAX_STAGE2_INTERVALS * params.a2s_points_per_interval;
    const int period = idx / samples_per_period;
    const int sample_in_period = idx % samples_per_period;
    const int interval = sample_in_period / params.a2s_points_per_interval;
    const int sample_in_interval = sample_in_period % params.a2s_points_per_interval;
    
    const double period_start = period * params.switching_period;
    const Stage2ScheduleGpu& schedule = schedules[period];
    
    // Compute time point like v2: t_start + duration * alpha
    const double t_start = schedule.boundaries[interval];
    const double t_end = schedule.boundaries[interval + 1];
    const double duration = t_end - t_start;
    const double alpha = (params.a2s_points_per_interval == 1) ? 
        0.0 : static_cast<double>(sample_in_interval) / static_cast<double>(params.a2s_points_per_interval - 1);
    const double t = t_start + duration * alpha;
    const double t_local = t - period_start;
    
    // Step 1: Calculate average model values (compute steady-state ONCE at t=0.0)
    double D_d_0, D_q_0;
    compute_dq_duties_device(params, 0.0, D_d_0, D_q_0);
    
    double x_dq_ss[8];
    solve_two_level_stage2_steady_state_device(params, params.boost_duty, D_d_0, D_q_0, x_dq_ss);
    
    double x_abc_avg_start[MAX_A2S_STATES];
    convert_dq_to_abc_device(x_dq_ss, params, period_start, x_abc_avg_start);
    
    // Step 2: A2S simulation
    double x_a2s[MAX_A2S_STATES];
    for (int i = 0; i < params.num_states; ++i) {
        x_a2s[i] = x_abc_avg_start[i];
    }
    
    if (t > period_start + EPS_TIME) {
        double current_t = period_start;
        const double target_t = t;
        
        int current_interval = 0;
        for (int i = 0; i < MAX_STAGE2_INTERVALS; ++i) {
            if (schedule.boundaries[i] <= current_t && current_t < schedule.boundaries[i + 1]) {
                current_interval = i;
                break;
            }
        }
        
        while (current_t < target_t - EPS_TIME) {
            // Get switching state from current interval (like v2 does)
            // v2 uses the switching state from the current interval for the entire step,
            // even if the step crosses a boundary
            const double current_t_local = current_t - period_start;
            int sboost, sa, sb, sc;
            get_switching_states_two_level_stage2(schedule, current_t_local, sboost, sa, sb, sc);
            TwoLevelStage2DerivativeFunctorDevice derivative{params, sboost, sa, sb, sc};
            
            const double interval_end = (schedule.boundaries[current_interval + 1] < target_t) ? 
                schedule.boundaries[current_interval + 1] : target_t;
            const double interval_duration = interval_end - current_t;
            
            if (interval_duration > EPS_TIME) {
                // Use step size matching v2's a2s_points_per_interval approach
                const int steps_in_interval = params.a2s_points_per_interval;
                const double h = interval_duration / steps_in_interval;
                
                for (int step = 0; step < steps_in_interval; ++step) {
                    const double t_step = current_t + step * h;
                    // Use the same switching state for all steps in this interval segment
                    rk4_step_device(x_a2s, t_step, h, params.num_states, derivative);
                }
            }
            
            // Advance to next interval or target
            current_t = interval_end;
            // Advance interval if we've reached the boundary (with tolerance for floating point)
            if (current_interval < MAX_STAGE2_INTERVALS - 1) {
                if (current_t >= schedule.boundaries[current_interval + 1] - EPS_TIME) {
                    current_interval++;
                }
            }
        }
    }
    
    output_time_points[idx] = t;
    for (int i = 0; i < params.num_states; ++i) {
        output_states[idx * params.num_states + i] = x_a2s[i];
    }
    
    int sboost, sa, sb, sc;
    get_switching_states_two_level_stage2(schedule, t_local, sboost, sa, sb, sc);
    output_switching_states[idx * 3 + 0] = sa;
    output_switching_states[idx * 3 + 1] = sb;
    output_switching_states[idx * 3 + 2] = sc;
}

__global__ void unified_kernel_three_level_stage1(SimulationParameters params,
                                                   const Stage1ScheduleGpu* schedules,
                                                   int num_samples,
                                                   double dt,
                                                   double* output_states,
                                                   double* output_time_points,
                                                   int* output_switching_states) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_samples) {
        return;
    }
    
    // Calculate which period and sample within period (matching v2's approach)
    const int samples_per_period = MAX_STAGE1_INTERVALS * params.a2s_points_per_interval;
    const int period = idx / samples_per_period;
    const int sample_in_period = idx % samples_per_period;
    const int interval = sample_in_period / params.a2s_points_per_interval;
    const int sample_in_interval = sample_in_period % params.a2s_points_per_interval;
    
    const double period_start = period * params.switching_period;
    const Stage1ScheduleGpu& schedule = schedules[period];
    
    // Compute time point like v2: t_start + duration * alpha
    const double t_start = schedule.boundaries[interval];
    const double t_end = schedule.boundaries[interval + 1];
    const double duration = t_end - t_start;
    const double alpha = (params.a2s_points_per_interval == 1) ? 
        0.0 : static_cast<double>(sample_in_interval) / static_cast<double>(params.a2s_points_per_interval - 1);
    const double t = t_start + duration * alpha;
    const double t_local = t - period_start;
    
    // Step 1: Calculate average model values (compute steady-state ONCE at t=0.0)
    double D_d_0, D_q_0;
    compute_dq_duties_device(params, 0.0, D_d_0, D_q_0);
    
    double x_dq_ss[8];
    solve_three_level_stage1_steady_state_device(params, D_d_0, D_q_0, x_dq_ss);
    
    double x_abc_avg_start[MAX_A2S_STATES];
    convert_dq_to_abc_device(x_dq_ss, params, period_start, x_abc_avg_start);
    
    // Step 2: A2S simulation
    double x_a2s[MAX_A2S_STATES];
    for (int i = 0; i < params.num_states; ++i) {
        x_a2s[i] = x_abc_avg_start[i];
    }
    
    if (t > period_start + EPS_TIME) {
        double current_t = period_start;
        const double target_t = t;
        
        int current_interval = 0;
        for (int i = 0; i < MAX_STAGE1_INTERVALS; ++i) {
            if (schedule.boundaries[i] <= current_t && current_t < schedule.boundaries[i + 1]) {
                current_interval = i;
                break;
            }
        }
        
        while (current_t < target_t - EPS_TIME) {
            // Get switching state from current interval (like v2 does)
            const double current_t_local = current_t - period_start;
            int sag, sbg, scg;
            get_switching_states_three_level_stage1(schedule, current_t_local, sag, sbg, scg);
            ThreeLevelStage1DerivativeFunctorDevice derivative{params, sag, sbg, scg};
            
            const double interval_end = (schedule.boundaries[current_interval + 1] < target_t) ? 
                schedule.boundaries[current_interval + 1] : target_t;
            const double interval_duration = interval_end - current_t;
            
            if (interval_duration > EPS_TIME) {
                // Use step size matching v2's a2s_points_per_interval approach
                const int steps_in_interval = params.a2s_points_per_interval;
                const double h = interval_duration / steps_in_interval;
                
                for (int step = 0; step < steps_in_interval; ++step) {
                    const double t_step = current_t + step * h;
                    // Use the same switching state for all steps in this interval segment
                    rk4_step_device(x_a2s, t_step, h, params.num_states, derivative);
                }
            }
            
            // Advance to next interval or target
            current_t = interval_end;
            // Advance interval if we've reached the boundary (with tolerance for floating point)
            if (current_interval < MAX_STAGE1_INTERVALS - 1) {
                if (current_t >= schedule.boundaries[current_interval + 1] - EPS_TIME) {
                    current_interval++;
                }
            }
        }
    }
    
    output_time_points[idx] = t;
    for (int i = 0; i < params.num_states; ++i) {
        output_states[idx * params.num_states + i] = x_a2s[i];
    }
    
    int sag, sbg, scg;
    get_switching_states_three_level_stage1(schedule, t_local, sag, sbg, scg);
    output_switching_states[idx * 3 + 0] = sag;
    output_switching_states[idx * 3 + 1] = sbg;
    output_switching_states[idx * 3 + 2] = scg;
}

__global__ void unified_kernel_three_level_stage2(SimulationParameters params,
                                                  const Stage2ScheduleGpu* schedules,
                                                  int num_samples,
                                                  double dt,
                                                  double* output_states,
                                                  double* output_time_points,
                                                  int* output_switching_states) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_samples) {
        return;
    }
    
    // Calculate which period and sample within period (matching v2's approach)
    const int samples_per_period = MAX_STAGE2_INTERVALS * params.a2s_points_per_interval;
    const int period = idx / samples_per_period;
    const int sample_in_period = idx % samples_per_period;
    const int interval = sample_in_period / params.a2s_points_per_interval;
    const int sample_in_interval = sample_in_period % params.a2s_points_per_interval;
    
    const double period_start = period * params.switching_period;
    const Stage2ScheduleGpu& schedule = schedules[period];
    
    // Compute time point like v2: t_start + duration * alpha
    const double t_start = schedule.boundaries[interval];
    const double t_end = schedule.boundaries[interval + 1];
    const double duration = t_end - t_start;
    const double alpha = (params.a2s_points_per_interval == 1) ? 
        0.0 : static_cast<double>(sample_in_interval) / static_cast<double>(params.a2s_points_per_interval - 1);
    const double t = t_start + duration * alpha;
    const double t_local = t - period_start;
    
    // Step 1: Calculate average model values (compute steady-state ONCE at t=0.0)
    double D_d_0, D_q_0;
    compute_dq_duties_device(params, 0.0, D_d_0, D_q_0);
    
    double x_dq_ss[8];
    solve_three_level_stage2_steady_state_device(params, params.boost_duty, D_d_0, D_q_0, x_dq_ss);
    
    double x_abc_avg_start[MAX_A2S_STATES];
    convert_dq_to_abc_device(x_dq_ss, params, period_start, x_abc_avg_start);
    
    // Step 2: A2S simulation
    double x_a2s[MAX_A2S_STATES];
    for (int i = 0; i < params.num_states; ++i) {
        x_a2s[i] = x_abc_avg_start[i];
    }
    
    if (t > period_start + EPS_TIME) {
        double current_t = period_start;
        const double target_t = t;
        
        int current_interval = 0;
        for (int i = 0; i < MAX_STAGE2_INTERVALS; ++i) {
            if (schedule.boundaries[i] <= current_t && current_t < schedule.boundaries[i + 1]) {
                current_interval = i;
                break;
            }
        }
        
        while (current_t < target_t - EPS_TIME) {
            // Get switching state from current interval (like v2 does)
            const double current_t_local = current_t - period_start;
            int sboost, sag, sbg, scg;
            get_switching_states_three_level_stage2(schedule, current_t_local, sboost, sag, sbg, scg);
            ThreeLevelStage2DerivativeFunctorDevice derivative{params, sboost, sag, sbg, scg};
            
            const double interval_end = (schedule.boundaries[current_interval + 1] < target_t) ? 
                schedule.boundaries[current_interval + 1] : target_t;
            const double interval_duration = interval_end - current_t;
            
            if (interval_duration > EPS_TIME) {
                // Use step size matching v2's a2s_points_per_interval approach
                const int steps_in_interval = params.a2s_points_per_interval;
                const double h = interval_duration / steps_in_interval;
                
                for (int step = 0; step < steps_in_interval; ++step) {
                    const double t_step = current_t + step * h;
                    // Use the same switching state for all steps in this interval segment
                    rk4_step_device(x_a2s, t_step, h, params.num_states, derivative);
                }
            }
            
            // Advance to next interval or target
            current_t = interval_end;
            // Advance interval if we've reached the boundary (with tolerance for floating point)
            if (current_interval < MAX_STAGE2_INTERVALS - 1) {
                if (current_t >= schedule.boundaries[current_interval + 1] - EPS_TIME) {
                    current_interval++;
                }
            }
        }
    }
    
    output_time_points[idx] = t;
    for (int i = 0; i < params.num_states; ++i) {
        output_states[idx * params.num_states + i] = x_a2s[i];
    }
    
    int sboost, sag, sbg, scg;
    get_switching_states_three_level_stage2(schedule, t_local, sboost, sag, sbg, scg);
    output_switching_states[idx * 3 + 0] = sag;
    output_switching_states[idx * 3 + 1] = sbg;
    output_switching_states[idx * 3 + 2] = scg;
}

// Helper functions for schedule creation (from v2)
std::vector<double> assemble_boundaries(const std::vector<double>& switch_times,
                                        double switching_period) {
    std::vector<double> boundaries;
    boundaries.reserve(switch_times.size() + 2);
    boundaries.push_back(0.0);
    for (double time : switch_times) {
        double clamped = std::clamp(time, 0.0, switching_period);
        clamped = std::max(clamped, boundaries.back());
        boundaries.push_back(clamped);
    }
    if (switching_period < boundaries.back()) {
        boundaries.push_back(boundaries.back());
    } else {
        boundaries.push_back(switching_period);
    }
    return boundaries;
}

int find_boost_index(const std::vector<double>& sorted_times, double boost_time) {
    for (std::size_t i = 0; i < sorted_times.size(); ++i) {
        if (std::abs(sorted_times[i] - boost_time) < 1e-10) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

Stage1ScheduleGpu make_stage1_schedule(const SimulationParameters& params,
                                       double period_start,
                                       const std::array<double, 6>& switch_times,
                                       const std::array<std::array<int, 3>, 7>& switching_states) {
    Stage1ScheduleGpu schedule{};

    const std::vector<double> times_vec(switch_times.begin(), switch_times.end());
    const auto boundaries = assemble_boundaries(times_vec, params.switching_period);

    for (int i = 0; i < MAX_STAGE1_INTERVALS + 1; ++i) {
        schedule.boundaries[i] = period_start + boundaries[static_cast<std::size_t>(i)];
    }
    for (int interval = 0; interval < MAX_STAGE1_INTERVALS; ++interval) {
        for (int leg = 0; leg < 3; ++leg) {
            schedule.inverter[interval][leg] = switching_states[static_cast<std::size_t>(interval)][leg];
        }
    }
    return schedule;
}

Stage2ScheduleGpu make_stage2_schedule(const SimulationParameters& params,
                                       double period_start,
                                       const std::array<std::array<int, 3>, 7>& switching_states,
                                       const std::array<double, 6>& switch_times,
                                       double boost_time) {
    Stage2ScheduleGpu schedule{};

    std::vector<double> all_times(switch_times.begin(), switch_times.end());
    all_times.push_back(boost_time);
    std::sort(all_times.begin(), all_times.end());
    const int boost_index = find_boost_index(all_times, boost_time);

    const auto boundaries = assemble_boundaries(all_times, params.switching_period);
    for (int i = 0; i < MAX_STAGE2_INTERVALS + 1; ++i) {
        schedule.boundaries[i] = period_start + boundaries[static_cast<std::size_t>(i)];
    }

    for (int interval = 0; interval < MAX_STAGE2_INTERVALS; ++interval) {
        if (interval <= boost_index) {
            schedule.sboost[interval] = 1;
            const int idx = std::min(interval, 6);
            for (int leg = 0; leg < 3; ++leg) {
                schedule.inverter[interval][leg] = switching_states[static_cast<std::size_t>(idx)][leg];
            }
        } else {
            schedule.sboost[interval] = 0;
            const int idx = std::min(interval - 1, 6);
            for (int leg = 0; leg < 3; ++leg) {
                schedule.inverter[interval][leg] = switching_states[static_cast<std::size_t>(idx)][leg];
            }
        }
    }

    return schedule;
}

int compute_period_count(const SimulationParameters& params) {
    const double periods_double = std::ceil(params.simulation_time / params.switching_period);
    const int periods = std::max(1, static_cast<int>(periods_double));
    return periods;
}

// Host-side helper: dq to abc conversion
void dq_to_abc_host(double d_val, double q_val, double theta, 
                    double& a, double& b, double& c) {
    const double PI = 3.14159265358979323846;
    a = d_val * std::sin(theta) + q_val * std::cos(theta);
    b = d_val * std::sin(theta - 2.0 * PI / 3.0) + q_val * std::cos(theta - 2.0 * PI / 3.0);
    c = d_val * std::sin(theta + 2.0 * PI / 3.0) + q_val * std::cos(theta + 2.0 * PI / 3.0);
}

// Host-side helper: convert dq steady-state to abc at time t
std::vector<double> convert_dq_to_abc_host(const std::vector<double>& x_dq,
                                           const SimulationParameters& params,
                                           double t) {
    const double PI = 3.14159265358979323846;
    const double w = 2.0 * PI * params.vg_freq;
    const double theta = w * t;
    
    std::vector<double> x_abc(params.num_states);
    
    if (params.model_stage == 1) {
        // Stage 1: [I1d, I1q, I2d, I2q, Vcd, Vcq] -> [I1a, I1b, I1c, I2a, I2b, I2c, Vca, Vcb, Vcc]
        double i1a, i1b, i1c, i2a, i2b, i2c, vca, vcb, vcc;
        dq_to_abc_host(x_dq[0], x_dq[1], theta, i1a, i1b, i1c);
        dq_to_abc_host(x_dq[2], x_dq[3], theta, i2a, i2b, i2c);
        dq_to_abc_host(x_dq[4], x_dq[5], theta, vca, vcb, vcc);
        
        x_abc[0] = i1a; x_abc[1] = i1b; x_abc[2] = i1c;
        x_abc[3] = i2a; x_abc[4] = i2b; x_abc[5] = i2c;
        x_abc[6] = vca; x_abc[7] = vcb; x_abc[8] = vcc;
    } else if (params.topology_level == 2) {
        // Stage 2 (2-level): [I1d, I1q, I2d, I2q, Vcd, Vcq, ILb, Vdc]
        double i1a, i1b, i1c, i2a, i2b, i2c, vca, vcb, vcc;
        dq_to_abc_host(x_dq[0], x_dq[1], theta, i1a, i1b, i1c);
        dq_to_abc_host(x_dq[2], x_dq[3], theta, i2a, i2b, i2c);
        dq_to_abc_host(x_dq[4], x_dq[5], theta, vca, vcb, vcc);
        
        x_abc[0] = i1a; x_abc[1] = i1b; x_abc[2] = i1c;
        x_abc[3] = i2a; x_abc[4] = i2b; x_abc[5] = i2c;
        x_abc[6] = vca; x_abc[7] = vcb; x_abc[8] = vcc;
        x_abc[9] = x_dq[6];  // IL_boost
        x_abc[10] = x_dq[7]; // Vdc
    } else {
        // Stage 2 (3-level): [I1d, I1q, I2d, I2q, Vcd, Vcq, ILb, Vdc]
        double i1a, i1b, i1c, i2a, i2b, i2c, vca, vcb, vcc;
        dq_to_abc_host(x_dq[0], x_dq[1], theta, i1a, i1b, i1c);
        dq_to_abc_host(x_dq[2], x_dq[3], theta, i2a, i2b, i2c);
        dq_to_abc_host(x_dq[4], x_dq[5], theta, vca, vcb, vcc);
        
        x_abc[0] = i1a; x_abc[1] = i1b; x_abc[2] = i1c;
        x_abc[3] = i2a; x_abc[4] = i2b; x_abc[5] = i2c;
        x_abc[6] = vca; x_abc[7] = vcb; x_abc[8] = vcc;
        x_abc[9] = x_dq[6];  // IL_boost
        x_abc[10] = x_dq[7] / 2.0; // Vdc1
        x_abc[11] = x_dq[7] / 2.0; // Vdc2
    }
    
    return x_abc;
}

// Apply calibration to outputs (host-side, similar to v2's device function)
void apply_calibration_host(const SimulationParameters& params,
                           UnifiedOutputs& outputs,
                           const std::vector<Stage1ScheduleGpu>& schedules_stage1,
                           const std::vector<Stage2ScheduleGpu>& schedules_stage2,
                           int intervals_per_period,
                           int samples_per_interval,
                           int samples_per_period,
                           int periods) {
    // For each period, apply calibration
    for (int period = 0; period < periods; ++period) {
        const int period_start_idx = period * samples_per_period;
        const int period_end_idx = period_start_idx + samples_per_period;
        
        // Get interval durations from schedule
        std::vector<double> interval_durations(intervals_per_period);
        if (params.model_stage == 1) {
            const Stage1ScheduleGpu& schedule = schedules_stage1[period];
            for (int i = 0; i < intervals_per_period; ++i) {
                interval_durations[i] = schedule.boundaries[i + 1] - schedule.boundaries[i];
            }
        } else {
            const Stage2ScheduleGpu& schedule = schedules_stage2[period];
            for (int i = 0; i < intervals_per_period; ++i) {
                interval_durations[i] = schedule.boundaries[i + 1] - schedule.boundaries[i];
            }
        }
        
        // Compute average model values for this period
        // We need to compute steady-state dq values, then convert to abc at each sample time
        // For now, let's use a simplified approach: we'll compute it inline
        
        // Compute duties at t=0.0 (same as kernel)
        const double t0 = 0.0;
        const double w = 2.0 * PI * params.vg_freq;
        const double theta0 = w * t0;
        
        // Get modulation parameters (use correct field names)
        double m, phi;
        const double v_ref_mag = params.reference_phase_magnitude;
        const double v_dc_actual = (params.model_stage == 1) ? params.v_pv : params.vdc_target;
        const double v_dc_effective = (params.topology_level == 3) ? (v_dc_actual / 2.0) : v_dc_actual;
        
        if (params.modulation == ModulationType::SVM) {
            m = v_ref_mag / (v_dc_effective * 0.612); // SVM modulation index
        } else {
            m = v_ref_mag / (v_dc_effective * 0.5);  // SPWM modulation index
        }
        phi = params.reference_phase_shift;
        
        const double D_d_0 = m * std::cos(phi - theta0);
        const double D_q_0 = m * std::sin(phi - theta0);
        
        // Compute steady-state solution (we need host-side solver)
        // For now, let's skip calibration and add a TODO
        // Actually, let's implement a basic version that at least computes the correction
        
        // Apply calibration for each state
        const int state_end = (params.model_stage == 1) ? 8 : params.num_states - 1;
        for (int state_idx = 0; state_idx <= state_end; ++state_idx) {
            if (state_idx >= params.num_states) continue;
            
            // Compute interval means from A2S outputs
            std::vector<double> interval_means(intervals_per_period, 0.0);
            for (int interval = 0; interval < intervals_per_period; ++interval) {
                double sum = 0.0;
                const int base_sample = period_start_idx + interval * samples_per_interval;
                for (int sample = 0; sample < samples_per_interval; ++sample) {
                    const int sample_idx = base_sample + sample;
                    if (sample_idx < period_end_idx) {
                        sum += outputs.states[sample_idx * params.num_states + state_idx];
                    }
                }
                interval_means[interval] = sum / static_cast<double>(samples_per_interval);
            }
            
            // Compute weighted mean
            double weighted_mean = 0.0;
            const double period_duration = params.switching_period;
            for (int interval = 0; interval < intervals_per_period; ++interval) {
                weighted_mean += interval_means[interval] * (interval_durations[interval] / period_duration);
            }
            
        // Compute average model mean
        // For DC quantities (ILboost, Vdc), the average model value is constant (steady-state dq value)
        // For AC quantities (I1, I2, Vc), we'd need to compute at each sample time and average
        // Since v2's calibration focuses on ILboost and Vdc for stage 2, we'll use the initial value
        // which comes from the average model steady-state solution
        
        double avg_mean = 0.0;
        if (period_start_idx < static_cast<int>(outputs.states.size() / params.num_states)) {
            // Use the initial value from the first sample of this period
            // This comes from the average model steady-state solution converted to abc
            // For DC quantities (state_idx >= 9 for stage 2), this is the correct average model value
            // For AC quantities, this is approximate but should still help
            avg_mean = outputs.states[period_start_idx * params.num_states + state_idx];
        } else {
            continue; // Skip if index is out of bounds
        }
        
        // Apply correction
        const double correction = avg_mean - weighted_mean;
        // Apply correction to all samples in this period
        for (int sample = period_start_idx; sample < period_end_idx; ++sample) {
            outputs.states[sample * params.num_states + state_idx] += correction;
        }
        }
    }
}

} // namespace

// Device functions moved outside anonymous namespace for external linking
// Device function to get boost switch state at a given time
__device__ double get_boost_switch_state_device(double t, const SimulationParameters& params) {
    if (params.model_stage == 1) {
        return 0.0; // No boost stage for single-stage
    }
    
    const double t_mod = fmod(t, params.switching_period);
    const double boost_on_time = params.boost_duty * params.switching_period;
    return (t_mod < boost_on_time) ? 1.0 : 0.0;
}

// Device function to calculate stress waveforms from a2s simulation outputs
// This is called within the GPU kernel to compute stress for each sample
__device__ void calculate_stress_device(
    const SimulationParameters& params,
    const double* states,  // Current state vector
    double t,              // Current time
    int sa, int sb, int sc, // Switching states
    double& V_ce,          // Output: Voltage stress on Phase A Top IGBT (V)
    double& I_c,           // Output: Current stress on Phase A Top IGBT (A)
    double& I_cap          // Output: DC Link capacitor ripple current (A)
) {
    // Determine if 3-level topology
    bool is_3level = (params.topology_level == 3);
    
    // Extract I1 currents (inverter-side inductor currents)
    // State indices: [I1a, I1b, I1c, I2a, I2b, I2c, Vca, Vcb, Vcc, ...]
    // I1 is at indices 0, 1, 2
    const int I1a_idx = 0;
    const int I1b_idx = 1;
    const int I1c_idx = 2;
    
    // Extract ILboost for stage 2 (index 9)
    const int ILboost_idx = 9;
    
    // Extract Vdc for stage 2
    // For 2-level: index 10 is Vdc
    // For 3-level: indices 10 and 11 are Vdc1 and Vdc2
    const int Vdc_idx = 10;
    const int Vdc1_idx = 10;
    const int Vdc2_idx = 11;
    
    // Get I1 currents
    double I1a = states[I1a_idx];
    double I1b = states[I1b_idx];
    double I1c = states[I1c_idx];
    
    if (params.model_stage == 1) {
        // Single-stage calculation
        double Vdc_val = params.v_pv; // For single-stage, use PV voltage
        
        if (is_3level) {
            // 3-Level NPC single-stage
            // States should already be in [-1, 0, 1] format from the simulation
            // But handle [0, 1, 2] format if present
            int sa_local = sa;
            if (sa_local > 1 || sb > 1 || sc > 1) {
                // Input is [0, 1, 2] format, convert to [-1, 0, 1]
                sa_local = sa_local - 1;
            }
            
            // V_ce (Outer Top Q1): Conducts at State P(1), blocks Vdc/2 otherwise
            bool is_P = (sa_local == 1);
            V_ce = is_P ? 0.0 : (Vdc_val / 2.0);
            
            // I_c (Outer Top Q1): Current flows only at State P(1)
            I_c = is_P ? fabs(I1a) : 0.0;
            
            // I_draw (Top Node): Current drawn from positive rail only at State P(1)
            int sb_local = (sb > 1) ? sb - 1 : sb;
            int sc_local = (sc > 1) ? sc - 1 : sc;
            double I_draw = ((sa_local == 1) ? I1a : 0.0) + 
                            ((sb_local == 1) ? I1b : 0.0) + 
                            ((sc_local == 1) ? I1c : 0.0);
            
            // I_cap_est: Will be adjusted later for single-stage
            I_cap = I_draw;
        } else {
            // 2-Level single-stage
            // V_ce (Top Q1): 0(OFF)->Vdc, 1(ON)->0
            V_ce = (sa == 0) ? Vdc_val : 0.0;
            
            // I_c (Top Q1): 1(ON)->|Current|
            I_c = (sa == 1) ? fabs(I1a) : 0.0;
            
            // I_draw (DC Link): Current drawn whenever top switch is ON
            double I_draw = (sa == 1 ? I1a : 0.0) + 
                            (sb == 1 ? I1b : 0.0) + 
                            (sc == 1 ? I1c : 0.0);
            
            I_cap = I_draw; // Will be adjusted later for single-stage
        }
    } else {
        // Two-stage calculation
        double sboost = get_boost_switch_state_device(t, params);
        
        // Get ILboost
        double ILboost = states[ILboost_idx];
        
        if (is_3level) {
            // 3-Level NPC two-stage
            // States should already be in [-1, 0, 1] format from the simulation
            // But handle [0, 1, 2] format if present
            int sa_local = sa;
            int sb_local = sb;
            int sc_local = sc;
            if (sa_local > 1 || sb_local > 1 || sc_local > 1) {
                // Input is [0, 1, 2] format, convert to [-1, 0, 1]
                sa_local = sa_local - 1;
                sb_local = sb_local - 1;
                sc_local = sc_local - 1;
            }
            
            // Get Vdc1 and Vdc2
            double Vdc1 = states[Vdc1_idx];
            double Vdc2 = states[Vdc2_idx];
            double V_total = Vdc1 + Vdc2;
            double V_blocking = V_total / 2.0;
            
            // V_ce (Outer Top Q1)
            bool is_P = (sa_local == 1);
            V_ce = is_P ? 0.0 : V_blocking;
            
            // I_c (Outer Top Q1)
            I_c = is_P ? fabs(I1a) : 0.0;
            
            // I_cap (Top Capacitor C1)
            // Input: Boost Diode Current (Flows into Top Node)
            double I_in = (1.0 - sboost) * ILboost;
            // Output: Inverter Draw from Top Node (State P)
            double I_draw = ((sa_local == 1) ? I1a : 0.0) + 
                            ((sb_local == 1) ? I1b : 0.0) + 
                            ((sc_local == 1) ? I1c : 0.0);
            I_cap = I_in - I_draw;
        } else {
            // 2-Level two-stage
            double V_total = states[Vdc_idx];
            
            // V_ce
            V_ce = (sa == 0) ? V_total : 0.0;
            
            // I_c
            I_c = (sa == 1) ? fabs(I1a) : 0.0;
            
            // I_cap (Total DC Link Cap)
            double I_in = (1.0 - sboost) * ILboost;
            double I_draw = (sa == 1 ? I1a : 0.0) + 
                            (sb == 1 ? I1b : 0.0) + 
                            (sc == 1 ? I1c : 0.0);
            I_cap = I_in - I_draw;
        }
    }
}

namespace {

void quantize_to_float_precision(std::vector<double>& values) {
    for (double& value : values) {
        value = static_cast<double>(static_cast<float>(value));
    }
}

} // namespace

UnifiedOutputs run_unified_gpu(const SimulationParameters& params) {
    return run_unified_gpu(params, ComputePrecision::Double);
}

UnifiedOutputs run_unified_gpu(const SimulationParameters& params, ComputePrecision precision) {
    const int periods = compute_period_count(params);
    
    // Match v2's output rate: periods * intervals_per_period * a2s_points_per_interval
    const int intervals_per_period = (params.model_stage == 1) ? MAX_STAGE1_INTERVALS : MAX_STAGE2_INTERVALS;
    const int samples_per_interval = params.a2s_points_per_interval;
    const int samples_per_period = intervals_per_period * samples_per_interval;
    const int num_samples = periods * samples_per_period;
    const double dt = params.switching_period / static_cast<double>(samples_per_period);
    
    if (samples_per_period <= 0) {
        throw std::runtime_error("Computed non-positive samples_per_period for A2S GPU solver");
    }
    
    UnifiedOutputs outputs{};
    outputs.num_states = params.num_states;
    outputs.total_samples = num_samples;
    outputs.states.assign(static_cast<std::size_t>(num_samples) * params.num_states, 0.0);
    outputs.time_points.assign(num_samples, 0.0);
    outputs.switching_states.assign(static_cast<std::size_t>(num_samples) * 3, 0);
    
    // Create schedules for all periods
    std::vector<Stage1ScheduleGpu> schedules_stage1;
    std::vector<Stage2ScheduleGpu> schedules_stage2;
    
    if (params.model_stage == 1) {
        schedules_stage1.reserve(periods);
        for (int period = 0; period < periods; ++period) {
            const double period_start = period * params.switching_period;
            std::array<double, 6> switch_times{};
            std::array<std::array<int, 3>, 7> switching_states{};
            
            if (params.topology_level == 2) {
                if (params.modulation == ModulationType::SVM) {
                    const auto sched = compute_svm_schedule(params, period_start);
                    switch_times = sched.switch_times;
                    switching_states = sched.states;
                } else {
                    const auto sched = compute_spwm_schedule(params, period_start);
                    switch_times = sched.switch_times;
                    switching_states = sched.states;
                }
            } else {
                ThreeLevelSchedule schedule{};
                if (params.modulation == ModulationType::SVM) {
                    schedule = compute_three_level_svm_schedule(params, period_start);
                } else {
                    schedule = compute_three_level_spwm_schedule(params, period_start);
                }
                switch_times = schedule.switch_times;
                switching_states = schedule.states;
            }
            
            schedules_stage1.push_back(make_stage1_schedule(params, period_start, switch_times, switching_states));
        }
    } else {
        schedules_stage2.reserve(periods);
        const double boost_time = std::clamp(compute_boost_schedule(params).boost_switch_time, 0.0, params.switching_period);
        
        for (int period = 0; period < periods; ++period) {
            const double period_start = period * params.switching_period;
            std::array<double, 6> switch_times{};
            std::array<std::array<int, 3>, 7> switching_states{};
            
            if (params.topology_level == 2) {
                if (params.modulation == ModulationType::SVM) {
                    const auto sched = compute_svm_schedule(params, period_start);
                    switch_times = sched.switch_times;
                    switching_states = sched.states;
                } else {
                    const auto sched = compute_spwm_schedule(params, period_start);
                    switch_times = sched.switch_times;
                    switching_states = sched.states;
                }
                
                schedules_stage2.push_back(make_stage2_schedule(params, period_start, switching_states, switch_times, boost_time));
            } else {
                ThreeLevelSchedule schedule{};
                if (params.modulation == ModulationType::SVM) {
                    schedule = compute_three_level_svm_schedule(params, period_start);
                } else {
                    schedule = compute_three_level_spwm_schedule(params, period_start);
                }
                switch_times = schedule.switch_times;
                switching_states = schedule.states;
                
                schedules_stage2.push_back(make_stage2_schedule(params, period_start, switching_states, switch_times, boost_time));
            }
        }
    }
    
    // Allocate device memory
    double* d_output_states = nullptr;
    double* d_time_points = nullptr;
    int* d_switching_states = nullptr;
    void* d_schedules = nullptr;
    
    const std::size_t states_bytes = outputs.states.size() * sizeof(double);
    const std::size_t time_bytes = outputs.time_points.size() * sizeof(double);
    const std::size_t switching_bytes = outputs.switching_states.size() * sizeof(int);
    
    // Start timing GPU operations (includes memory transfers and kernel execution)
    const auto clock_start = std::chrono::steady_clock::now();
    
    CUDA_CHECK(cudaMalloc(&d_output_states, states_bytes));
    CUDA_CHECK(cudaMalloc(&d_time_points, time_bytes));
    CUDA_CHECK(cudaMalloc(&d_switching_states, switching_bytes));
    
    if (params.model_stage == 1) {
        const std::size_t schedules_bytes = schedules_stage1.size() * sizeof(Stage1ScheduleGpu);
        CUDA_CHECK(cudaMalloc(&d_schedules, schedules_bytes));
        CUDA_CHECK(cudaMemcpy(d_schedules, schedules_stage1.data(), schedules_bytes, cudaMemcpyHostToDevice));
    } else {
        const std::size_t schedules_bytes = schedules_stage2.size() * sizeof(Stage2ScheduleGpu);
        CUDA_CHECK(cudaMalloc(&d_schedules, schedules_bytes));
        CUDA_CHECK(cudaMemcpy(d_schedules, schedules_stage2.data(), schedules_bytes, cudaMemcpyHostToDevice));
    }
    
    // Launch kernel
    const int threads_per_block = 256;
    const int blocks = (num_samples + threads_per_block - 1) / threads_per_block;
    
    if (params.topology_level == 2 && params.model_stage == 1) {
        unified_kernel_two_level_stage1<<<blocks, threads_per_block>>>(
            params,
            static_cast<const Stage1ScheduleGpu*>(d_schedules),
            num_samples,
            dt,
            d_output_states,
            d_time_points,
            d_switching_states);
    } else if (params.topology_level == 2 && params.model_stage == 2) {
        unified_kernel_two_level_stage2<<<blocks, threads_per_block>>>(
            params,
            static_cast<const Stage2ScheduleGpu*>(d_schedules),
            num_samples,
            dt,
            d_output_states,
            d_time_points,
            d_switching_states);
    } else if (params.topology_level == 3 && params.model_stage == 1) {
        unified_kernel_three_level_stage1<<<blocks, threads_per_block>>>(
            params,
            static_cast<const Stage1ScheduleGpu*>(d_schedules),
            num_samples,
            dt,
            d_output_states,
            d_time_points,
            d_switching_states);
    } else if (params.topology_level == 3 && params.model_stage == 2) {
        unified_kernel_three_level_stage2<<<blocks, threads_per_block>>>(
            params,
            static_cast<const Stage2ScheduleGpu*>(d_schedules),
            num_samples,
            dt,
            d_output_states,
            d_time_points,
            d_switching_states);
    } else {
        throw std::runtime_error("Unsupported topology/stage combination");
    }
    
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());
    
    // Copy results back (included in GPU time)
    CUDA_CHECK(cudaMemcpy(outputs.states.data(), d_output_states, states_bytes, cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(outputs.time_points.data(), d_time_points, time_bytes, cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(outputs.switching_states.data(), d_switching_states, switching_bytes, cudaMemcpyDeviceToHost));
    
    // Apply calibration (like v2 does) - adjust period-averaged values to match average model
    if (params.avg_points_per_period > 0) {
        apply_calibration_host(params, outputs, schedules_stage1, schedules_stage2, 
                              intervals_per_period, samples_per_interval, samples_per_period, periods);
    }

    if (precision == ComputePrecision::Float) {
        quantize_to_float_precision(outputs.states);
        quantize_to_float_precision(outputs.time_points);
    }
    
    const auto clock_end = std::chrono::steady_clock::now();
    outputs.elapsed_s = std::chrono::duration<double>(clock_end - clock_start).count();
    
    // Cleanup
    cudaFree(d_output_states);
    cudaFree(d_time_points);
    cudaFree(d_switching_states);
    cudaFree(d_schedules);
    
    return outputs;
}

// Batch processing function: process multiple cases in parallel using CUDA streams
BatchOutputs run_unified_gpu_batch(const std::vector<SimulationParameters>& params_batch) {
    return run_unified_gpu_batch(params_batch, ComputePrecision::Double);
}

BatchOutputs run_unified_gpu_batch(const std::vector<SimulationParameters>& params_batch,
                                   ComputePrecision precision) {
    if (params_batch.empty()) {
        throw std::runtime_error("Empty parameter batch");
    }
    
    const auto clock_start = std::chrono::steady_clock::now();
    
    BatchOutputs batch_outputs;
    batch_outputs.outputs.resize(params_batch.size());
    
    // Process each case (for now sequentially, but can be parallelized with streams)
    // Since each case uses the full GPU, we process them one at a time
    // but this function allows for future optimization with streams
    for (std::size_t i = 0; i < params_batch.size(); ++i) {
        batch_outputs.outputs[i] = run_unified_gpu(params_batch[i], precision);
    }
    
    const auto clock_end = std::chrono::steady_clock::now();
    batch_outputs.elapsed_s = std::chrono::duration<double>(clock_end - clock_start).count();
    
    return batch_outputs;
}
