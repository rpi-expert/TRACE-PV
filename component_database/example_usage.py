#!/usr/bin/env python3
"""
Example usage of ComponentDatabase
Python version of example_usage.cpp
"""

import sys
from pathlib import Path
from component_database import ComponentDatabase, CapacitorCoefficients, CapacitorType


def main():
    """Example usage of ComponentDatabase."""
    if len(sys.argv) < 2:
        print("Usage: python3 example_usage.py <database_path>")
        print("Example: python3 example_usage.py component_parameters.db")
        return 1
    
    db_path = sys.argv[1]
    
    # Initialize database
    db = ComponentDatabase()
    if not db.initialize(db_path):
        print(f"Error: Failed to initialize database: {db_path}")
        return 1
    
    print(f"Database initialized: {db_path}\n")
    
    # Example: Load capacitor
    cap_part = "Rubycon_475VXG330MEFCSN30X55"
    print(f"Loading capacitor: {cap_part}")
    
    cap_coeffs = CapacitorCoefficients()
    cap_type = CapacitorType.ALUMINUM_ELECTROLYTIC
    voltage_type = "DC"
    
    if db.load_capacitor(cap_part, cap_coeffs, cap_type, voltage_type):
        print(f"  Type: {cap_type.value}")
        print(f"  Voltage Type: {voltage_type}")
        if cap_type == CapacitorType.ALUMINUM_ELECTROLYTIC and cap_coeffs.aluminum:
            print(f"  L0: {cap_coeffs.aluminum.L0}")
            print(f"  V0: {cap_coeffs.aluminum.V0}")
            print(f"  T0: {cap_coeffs.aluminum.T0}")
        print(f"  Rth_amb: {cap_coeffs.rth_amb}")
        print(f"  Rth_surf: {cap_coeffs.rth_surf}")
        print(f"  ESR: {cap_coeffs.esr}")
    else:
        print(f"  Error: Capacitor not found")
    
    # Example: Load fan cooling
    fan_part = "NMB 09238RE-24N-GU"
    print(f"\nLoading fan cooling: {fan_part}")
    
    from component_database import FanCoefficients
    fan_coeffs = FanCoefficients()
    
    if db.load_fan_cooling(fan_part, fan_coeffs):
        print(f"  Electrical A: {fan_coeffs.electrical.A}")
        print(f"  Electrical n: {fan_coeffs.electrical.n}")
        print(f"  Electrical Ea: {fan_coeffs.electrical.Ea}")
        print(f"  Mechanical A: {fan_coeffs.mechanical.A}")
        print(f"  Mechanical c: {fan_coeffs.mechanical.c}")
    else:
        print(f"  Error: Fan cooling not found")
    
    # Example: Load power module
    pm_part = "IGBT001"
    print(f"\nLoading power module: {pm_part}")
    
    from component_database import PowerModuleCoefficients
    pm_coeffs = PowerModuleCoefficients()
    
    if db.load_power_module(pm_part, pm_coeffs):
        print(f"  A: {pm_coeffs.A}")
        print(f"  B: {pm_coeffs.B}")
        print(f"  C: {pm_coeffs.C}")
        print(f"  D: {pm_coeffs.D}")
        print(f"  Ea: {pm_coeffs.Ea}")
        print(f"  T_ref: {pm_coeffs.T_ref}")
    else:
        print(f"  Error: Power module not found")
    
    return 0


if __name__ == "__main__":
    sys.exit(main())

