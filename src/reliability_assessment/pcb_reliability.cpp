#include "pcb_reliability.h"
#include <cmath>
#include <algorithm>

// Calculate PCB cycles to failure (Nf) for a given delta_T using strain energy model
// Based on Python PCB_lifetime function
double calculate_pcb_nf(double delta_T, const PCBParameters& pcb_params) {
    // Use absolute value of delta_T
    double delta_T_abs = std::abs(delta_T);
    
    // Geometric data from PCB parameters
    const double component_l = pcb_params.component_dims.length;
    const double component_w = pcb_params.component_dims.width;
    const double component_h = pcb_params.component_dims.thickness;
    const double copper_l = pcb_params.copper_dims.length;
    const double copper_w = pcb_params.copper_dims.width;
    const double copper_h = pcb_params.copper_dims.thickness;
    const double solder_l = pcb_params.solder_dims.length;
    const double solder_w = pcb_params.solder_dims.width;
    const double solder_h = pcb_params.solder_dims.thickness;
    
    // Material properties
    const double CTE_comp = pcb_params.CTE_component;
    const double CTE_FR4 = pcb_params.CTE_FR4;
    const double E_comp = 310000.0;  // Elastic modulus of component (Pa) - from Python code
    const double E_FR4 = pcb_params.E_FR4;
    const double G_solder = pcb_params.shear_modulus;
    const double G_copper = pcb_params.G_copper;
    const double G_FR4 = pcb_params.G_FR4;
    const double Poisson_FR4 = pcb_params.Poisson_FR4;
    const double pcb_thickness = pcb_params.pcb_thickness;
    
    // Calculate CTE difference
    const double d_CTE = CTE_comp - CTE_FR4;
    
    // Strain Energy Model Calculation
    // Ld_SE = 0.5 * component_l (from Python: Ld_SE = 0.5 * Geo_data[0])
    const double Ld_SE = 0.5 * component_l;
    
    // Areas
    const double A_SE = solder_l * solder_w;  // Solder joint area
    const double A1_SE = component_h * component_w;  // Component cross-sectional area
    const double A2_SE = 2.0 * copper_w * pcb_thickness;  // PCB cross-sectional area
    const double Ab_SE = copper_l * copper_w;  // Copper pad area
    const double hb_SE = copper_h;  // Copper pad thickness
    const double As_SE = 0.75 * Ab_SE;  // Effective solder area
    const double a_SE = 0.5 * copper_l;  // Half copper pad length
    const double hs_SE = solder_h;  // Solder joint thickness
    
    // Calculate strain
    const double d_strain_SE = 1.38271485223961 * std::sqrt(Ld_SE * Ld_SE * Ld_SE / (A_SE * hs_SE)) * 
                               std::abs(d_CTE * delta_T_abs);
    
    // Calculate force
    const double Fnum_SE = (CTE_FR4 - CTE_comp) * delta_T_abs * Ld_SE;
    const double Fden_SE = Ld_SE / (E_comp * A1_SE) + 
                           Ld_SE / (E_FR4 * A2_SE) + 
                           hs_SE / (As_SE * G_solder) + 
                           hb_SE / (Ab_SE * G_copper) + 
                           (2.0 - Poisson_FR4) / (9.0 * G_FR4 * a_SE);
    const double F = Fnum_SE / Fden_SE;
    
    // Calculate shear stress
    const double Tau_SE = F / As_SE;
    
    // Calculate strain energy density
    const double dW_SE = Tau_SE * d_strain_SE;
    
    // Calculate cycles to failure using calibrated model
    // N_f50_SE = (adjust_param * dW_SE / 5920)^(-1.3)
    const double adjust_param = pcb_params.adjust_param;
    const double N_f50_SE = std::pow(adjust_param * dW_SE / 5920.0, -1.3);
    
    return N_f50_SE;
}

// Calculate PCB stressor from rainflow counting results
double calculate_pcb_stressor(
    const std::vector<double>& delta_range,
    const std::vector<double>& delta_cycle,
    const PCBParameters& pcb_params) {
    
    if (delta_range.empty() || delta_range.size() != delta_cycle.size()) {
        return 0.0;
    }
    
    double total_stressor = 0.0;
    
    // For each cycle from rainflow counting
    for (size_t i = 0; i < delta_range.size(); ++i) {
        double delta_T = delta_range[i];
        double cycles = delta_cycle[i];
        
        // Calculate Nf for this delta_T
        double Nf = calculate_pcb_nf(delta_T, pcb_params);
        
        // Accumulate stressor: cycles/Nf for each cycle type
        // This represents the fraction of lifetime consumed by these cycles
        if (Nf > 0.0) {
            total_stressor += cycles / Nf;
        }
    }
    
    return total_stressor;
}

