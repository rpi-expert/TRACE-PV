"""
Single Diode Model Solver
Python version of single_diode_model.cpp/h
"""

import math
from typing import List, Tuple
from dataclasses import dataclass


@dataclass
class SingleDiodeParams:
    """Single Diode Model Parameters extracted from PV panel JSON."""
    IL: float  # Light-generated current (A) at STC
    I0: float  # Diode saturation current (A)
    Rs: float  # Series resistance (Ω)
    Rsh: float  # Shunt resistance (Ω)
    Nmodule: float  # Number of modules
    Nstring: float  # Number of strings
    DI_factor: float  # Diode ideality factor
    Voc_stc: float  # Open circuit voltage at STC (V)
    Isc_stc: float  # Short circuit current at STC (A)
    Tk_Voc: float  # Temperature coefficient of Voc (%/°C)
    Tk_Isc: float  # Temperature coefficient of Isc (%/°C)
    
    # Reference conditions (STC)
    G_ref: float = 1000.0  # W/m²
    T_ref: float = 25.0  # °C
    q: float = 1.602176634e-19  # Elementary charge (C)
    k: float = 1.380649e-23  # Boltzmann constant (J/K)


@dataclass
class IVPoint:
    """IV Curve Point."""
    V: float  # Voltage (V)
    I: float  # Current (A)


class SingleDiodeModel:
    """Single Diode Model Solver using Newton-Raphson method."""
    
    @staticmethod
    def calculate_iv_curve(
        params: SingleDiodeParams,
        G: float,
        T: float,
        num_points: int = 30
    ) -> List[IVPoint]:
        """Calculate IV curve for given conditions."""
        try:
            # Calculate Voc and Isc first
            voc, isc = SingleDiodeModel.calculate_voc_isc(params, G, T)
            
            if voc <= 0 or isc <= 0:
                return []
            
            curve = []
            
            # Generate points from Voc to 0 (or slightly negative for full curve)
            # Use more points near the "knee" of the curve
            for i in range(num_points):
                # Use non-uniform spacing: more points near Voc (knee region)
                ratio = i / (num_points - 1) if num_points > 1 else 0
                
                # Apply non-linear spacing: x^2 gives more points near Voc
                v_ratio = 1.0 - ratio * ratio
                V = voc * v_ratio
                
                # Solve for current at this voltage
                try:
                    I = SingleDiodeModel.calculate_current(params, G, T, V)
                except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError):
                    I = 0.0
                
                # Clamp to physical limits
                I = max(0.0, min(I, isc))
                
                curve.append(IVPoint(V=V, I=I))
            
            # Sort by voltage (ascending)
            curve.sort(key=lambda p: p.V)
            
            return curve
        except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError):
            return []
    
    @staticmethod
    def calculate_voc_isc(
        params: SingleDiodeParams,
        G: float,
        T: float
    ) -> Tuple[float, float]:
        """Calculate Voc and Isc for given conditions."""
        try:
            # Adjust parameters for temperature and irradiance
            IL_adj, I0_adj, Vt = SingleDiodeModel._adjust_parameters(params, G, T)
            
            if IL_adj <= 0 or I0_adj <= 0 or Vt <= 0:
                return 0.0, 0.0
            
            # Short circuit current: I = IL when V = 0
            # At V=0, the diode term is small, so Isc ≈ IL
            isc = IL_adj
            
            # Open circuit voltage: I = 0 when V = Voc
            # 0 = IL - I0*(exp(Voc/(n*Vt)) - 1) - Voc/Rsh
            # For large Rsh, Voc ≈ (n*Vt)*ln(IL/I0 + 1)
            nVt = params.DI_factor * Vt
            if nVt <= 0 or IL_adj <= 0 or I0_adj <= 0:
                return 0.0, 0.0  # Invalid parameters
            
            try:
                ratio = IL_adj / I0_adj if I0_adj > 0 else 0.0
                if ratio <= 0:
                    voc = 0.0
                    return voc, isc
                voc = nVt * math.log(ratio + 1.0)
                if not (0 < voc < 1000):
                    voc = 0.0
                    return voc, isc
            except (ValueError, OverflowError, ZeroDivisionError):
                voc = 0.0
                return voc, isc
            
            # Refine Voc using Newton-Raphson
            V_guess = voc
            max_iter = 50
            tolerance = 1e-6
            
            for _ in range(max_iter):
                try:
                    I = SingleDiodeModel.calculate_current(params, G, T, V_guess)
                except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError):
                    break
                    
                if abs(I) < tolerance:
                    voc = V_guess
                    break
                
                # Derivative dI/dV at V_guess
                exp_arg = V_guess / nVt
                exp_arg = max(-700.0, min(700.0, exp_arg))
                
                try:
                    if exp_arg > 700.0:
                        exp_val = 1e304
                    elif exp_arg < -700.0:
                        exp_val = 0.0
                    else:
                        exp_val = math.exp(exp_arg)
                    
                    if nVt > 0:
                        dIdV = -I0_adj / nVt * exp_val
                    else:
                        dIdV = -1e10
                    
                    if params.Rsh > 0:
                        dIdV -= 1.0 / params.Rsh
                except (OverflowError, ValueError, ZeroDivisionError):
                    dIdV = -1e10  # Large negative value
                    
                if abs(dIdV) < 1e-12:
                    break
                
                try:
                    V_guess = V_guess - I / dIdV
                except (OverflowError, ValueError, ZeroDivisionError):
                    break
                
                if V_guess < 0:
                    V_guess = 0
                if V_guess > voc * 1.5:
                    V_guess = voc * 1.5
            
            voc = V_guess
            return voc, isc
        except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError):
            return 0.0, 0.0
    
    @staticmethod
    def calculate_current(
        params: SingleDiodeParams,
        G: float,
        T: float,
        V: float
    ) -> float:
        """Calculate current at a specific voltage."""
        try:
            # Use Newton-Raphson to solve for current
            IL_adj, I0_adj, Vt = SingleDiodeModel._adjust_parameters(params, G, T)
            
            if IL_adj <= 0 or I0_adj <= 0 or Vt <= 0:
                return 0.0
            
            # Initial guess: assume Rs is small, so I ≈ IL - I0*(exp(V/(n*Vt)) - 1) - V/Rsh
            nVt = params.DI_factor * Vt
            if nVt <= 0:
                return 0.0
            
            exp_arg = V / nVt
            exp_arg = max(-700.0, min(700.0, exp_arg))
            
            try:
                if exp_arg > 700.0:
                    exp_term = I0_adj * 1e304
                elif exp_arg < -700.0:
                    exp_term = -I0_adj
                else:
                    exp_term = I0_adj * (math.exp(exp_arg) - 1.0)
                
                if params.Rsh > 0:
                    I_guess = IL_adj - exp_term - V / params.Rsh
                else:
                    I_guess = IL_adj - exp_term
            except (OverflowError, ValueError, ZeroDivisionError):
                I_guess = IL_adj if exp_arg > 0 else IL_adj - I0_adj
            
            I_guess = max(0.0, min(I_guess, IL_adj))
            
            return SingleDiodeModel._solve_current_newton_raphson(params, G, T, V, I_guess)
        except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError):
            return 0.0
    
    @staticmethod
    def _diode_equation(
        V: float,
        I: float,
        params: SingleDiodeParams,
        G: float,
        T: float
    ) -> float:
        """Single diode model equation: I = IL - I0*(exp((V+I*Rs)/(n*Vt)) - 1) - (V+I*Rs)/Rsh"""
        IL_adj, I0_adj, Vt = SingleDiodeModel._adjust_parameters(params, G, T)
        
        nVt = params.DI_factor * Vt
        if nVt <= 0:
            return float('inf')  # Invalid parameters
        
        Vd = V + I * params.Rs
        
        term1 = IL_adj
        
        # Clamp exponent to prevent overflow (exp(700) is near max float)
        exp_arg = Vd / nVt
        exp_arg = max(-700.0, min(700.0, exp_arg))
        
        try:
            if exp_arg > 700.0:
                term2 = I0_adj * 1e304  # Very large value
            elif exp_arg < -700.0:
                term2 = -I0_adj  # exp(-700) ≈ 0, so exp - 1 ≈ -1
            else:
                term2 = I0_adj * (math.exp(exp_arg) - 1.0)
        except (OverflowError, ValueError):
            term2 = I0_adj * 1e304 if exp_arg > 0 else -I0_adj
        
        term3 = Vd / params.Rsh if params.Rsh > 0 else 0.0
        
        return I - term1 + term2 + term3
    
    @staticmethod
    def _diode_equation_derivative(
        V: float,
        I: float,
        params: SingleDiodeParams,
        G: float,
        T: float
    ) -> float:
        """Derivative of diode equation with respect to I."""
        IL_adj, I0_adj, Vt = SingleDiodeModel._adjust_parameters(params, G, T)
        
        nVt = params.DI_factor * Vt
        if nVt <= 0:
            return 1.0  # Invalid parameters, return default
        
        Vd = V + I * params.Rs
        
        # Clamp exponent to prevent overflow
        exp_arg = Vd / nVt
        exp_arg = max(-700.0, min(700.0, exp_arg))
        
        try:
            if exp_arg > 700.0:
                dterm2_dI = I0_adj * 1e304 * params.Rs / nVt if nVt > 0 else 1e10
            elif exp_arg < -700.0:
                dterm2_dI = 0.0
            else:
                dterm2_dI = I0_adj * math.exp(exp_arg) * params.Rs / nVt if nVt > 0 else 0.0
        except (OverflowError, ValueError, ZeroDivisionError):
            dterm2_dI = 1e10 if exp_arg > 0 else 0.0
        
        dterm3_dI = params.Rs / params.Rsh if params.Rsh > 0 else 0.0
        
        return 1.0 - dterm2_dI - dterm3_dI
    
    @staticmethod
    def _solve_current_newton_raphson(
        params: SingleDiodeParams,
        G: float,
        T: float,
        V: float,
        initial_guess: float
    ) -> float:
        """Newton-Raphson solver for current at given voltage."""
        try:
            I = initial_guess
            max_iter = 100
            tolerance = 1e-9
            
            for _ in range(max_iter):
                try:
                    residual = SingleDiodeModel._diode_equation(V, I, params, G, T)
                except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError):
                    break
                
                if abs(residual) < tolerance:
                    break
                
                try:
                    derivative = SingleDiodeModel._diode_equation_derivative(V, I, params, G, T)
                except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError):
                    break
                
                if abs(derivative) < 1e-12:
                    # Avoid division by zero
                    break
                
                try:
                    I_new = I - residual / derivative
                except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError):
                    break
                
                # Clamp to physical bounds
                I_new = max(0.0, I_new)
                
                # Check for convergence
                if abs(I_new - I) < tolerance:
                    I = I_new
                    break
                
                I = I_new
            
            return I
        except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError):
            return max(0.0, initial_guess)
    
    @staticmethod
    def _adjust_parameters(
        params: SingleDiodeParams,
        G: float,
        T: float
    ) -> Tuple[float, float, float]:
        """Adjust parameters for temperature and irradiance."""
        # Temperature in Kelvin
        T_kelvin = T + 273.15
        T_ref_kelvin = params.T_ref + 273.15
        
        # Thermal voltage
        Vt = params.k * T_kelvin / params.q
        
        # Adjust light-generated current for irradiance and temperature
        # IL = IL_stc * (G/G_ref) * (1 + Tk_Isc * (T - T_ref))
        IL_adj = params.IL * (G / params.G_ref) * \
                 (1.0 + params.Tk_Isc / 100.0 * (T - params.T_ref))
        
        # Adjust diode saturation current for temperature
        # I0 = I0_stc * (T/T_ref)^3 * exp((Eg*q/k) * (1/T_ref - 1/T))
        # For silicon: Eg = 1.12 eV, so Eg*q/k ≈ 13000 K
        Eg = 1.12  # Bandgap energy for silicon (eV)
        temp_factor = (T_kelvin / T_ref_kelvin) ** 3.0
        # Eg*q/k: convert Eg from eV to J, then divide by k
        # Eg*q/k = (1.12 * 1.602e-19) / 1.381e-23 ≈ 13000 K
        Eg_qk = (Eg * params.q) / params.k
        exp_arg = Eg_qk * (1.0 / T_ref_kelvin - 1.0 / T_kelvin)
        exp_arg = max(-700.0, min(700.0, exp_arg))
        
        try:
            if exp_arg > 700.0:
                exp_factor = 1e304  # Very large but finite value
            elif exp_arg < -700.0:
                exp_factor = 1e-304  # Very small but finite value
            else:
                exp_factor = math.exp(exp_arg)
        except (OverflowError, ValueError):
            exp_factor = 1e304 if exp_arg > 0 else 1e-304
        
        I0_adj = params.I0 * temp_factor * exp_factor
        
        return IL_adj, I0_adj, Vt

