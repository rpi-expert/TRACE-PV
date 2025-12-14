#!/usr/bin/env python3
"""
Offline Data Generator (Populator)
Python version of offline_data_generator.cpp

This tool reads PV panel JSON configuration files and generates
IV curves for a grid of conditions, storing them in the database.

Grid:
  Irradiance: 0 to 1000 W/m² (Step: 100)
  Temperature: 0 to 60 °C (Step: 5)
"""

import json
import sys
import os
from pathlib import Path
from typing import Tuple, List
# Handle both package and standalone imports
try:
    from .pv_performance_db import PVPerformanceDatabase
    from .single_diode_model import SingleDiodeParams, SingleDiodeModel, IVPoint
except ImportError:
    # For standalone execution
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).parent))
    from pv_performance_db import PVPerformanceDatabase
    from single_diode_model import SingleDiodeParams, SingleDiodeModel, IVPoint


def extract_json_double(json_str: str, key: str) -> float:
    """Extract double value from JSON string."""
    search_key = f'"{key}"'
    pos = json_str.find(search_key)
    if pos == -1:
        return 0.0
    
    pos = json_str.find(":", pos)
    if pos == -1:
        return 0.0
    pos += 1
    
    # Skip whitespace
    while pos < len(json_str) and json_str[pos].isspace():
        pos += 1
    
    # Find the end of the number
    end = pos
    while end < len(json_str) and (
        json_str[end].isdigit() or json_str[end] in '.-+eE'
    ):
        end += 1
    
    if end == pos:
        return 0.0
    
    try:
        num_str = json_str[pos:end].rstrip(',')
        return float(num_str)
    except ValueError:
        return 0.0


def extract_json_string(json_str: str, key: str) -> str:
    """Extract string value from JSON string."""
    search_key = f'"{key}"'
    pos = json_str.find(search_key)
    if pos == -1:
        return ""
    
    pos = json_str.find(":", pos)
    if pos == -1:
        return ""
    pos += 1
    
    # Skip whitespace
    while pos < len(json_str) and json_str[pos].isspace():
        pos += 1
    
    # Check for string (quoted)
    if pos < len(json_str) and json_str[pos] == '"':
        pos += 1
        end = json_str.find('"', pos)
        if end == -1:
            return ""
        return json_str[pos:end]
    
    return ""


def estimate_single_diode_params(params: SingleDiodeParams):
    """Estimate Single Diode Model parameters from IV curve data when missing."""
    import math
    
    # IL ≈ Isc (light current approximately equals short circuit current at STC)
    if params.IL <= 0 and params.Isc_stc > 0:
        params.IL = params.Isc_stc * 1.01  # Slightly higher to account for shunt losses
    
    # Default diode ideality factor (typical range: 1-2, use 1.3 for crystalline silicon)
    if params.DI_factor <= 0:
        params.DI_factor = 1.3
    
    # Estimate I0 from Voc using the diode equation approximation
    if params.I0 <= 0 and params.Voc_stc > 0 and params.IL > 0:
        try:
            T_kelvin = params.T_ref + 273.15
            Vt = params.k * T_kelvin / params.q
            if Vt <= 0:
                params.I0 = 1e-12  # Default value
            else:
                nVt = params.DI_factor * Vt
                if nVt <= 0:
                    params.I0 = 1e-12
                else:
                    exp_arg = params.Voc_stc / nVt
                    exp_arg = max(-700.0, min(700.0, exp_arg))
                    
                    try:
                        exp_val = math.exp(exp_arg)
                        if exp_val > 0:
                            params.I0 = params.IL / exp_val
                        else:
                            params.I0 = 1e-12
                    except (OverflowError, ValueError):
                        params.I0 = 1e-12
            
            # Ensure reasonable range
            params.I0 = max(1e-15, min(1e-6, params.I0))
        except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError):
            params.I0 = 1e-12  # Default value
    
    # Estimate Rsh from approximate slope near Isc
    if params.Rsh <= 0:
        params.Rsh = 200.0
        if params.Voc_stc > 0 and params.Isc_stc > 0:
            estimated_shunt_current = params.Isc_stc * 0.02  # 2% of Isc
            params.Rsh = params.Voc_stc / estimated_shunt_current
            params.Rsh = max(50.0, min(1000.0, params.Rsh))
    
    # Estimate Rs from approximate slope near Voc
    if params.Rs <= 0:
        params.Rs = 0.2
        if params.Voc_stc > 0 and params.Isc_stc > 0:
            params.Rs = 0.2
            params.Rs = max(0.01, min(2.0, params.Rs))
    
    # Default module/string configuration if missing
    if params.Nmodule <= 0:
        params.Nmodule = 1.0
    if params.Nstring <= 0:
        params.Nstring = 1.0


def parse_pv_json(json_path: str) -> Tuple[bool, SingleDiodeParams, str]:
    """Parse PV panel JSON file and extract Single Diode Model parameters."""
    try:
        with open(json_path, 'r') as f:
            json_content = f.read()
    except IOError as e:
        print(f"Error: Cannot open file {json_path}: {e}")
        return False, None, ""
    
    # Check for PVpanel key
    if '"PVpanel"' not in json_content:
        print(f"Error: Missing 'PVpanel' key in JSON")
        return False, None, ""
    
    # Extract part number
    part_number = extract_json_string(json_content, "Part Number")
    if not part_number:
        print(f"Error: Missing 'Part Number' in JSON")
        return False, None, ""
    
    # Extract Single Diode Model parameters
    params = SingleDiodeParams(
        IL=extract_json_double(json_content, "IL"),
        I0=extract_json_double(json_content, "I0"),
        Rs=extract_json_double(json_content, "Rs"),
        Rsh=extract_json_double(json_content, "Rsh"),
        Nmodule=extract_json_double(json_content, "Nmodule"),
        Nstring=extract_json_double(json_content, "Nstring"),
        DI_factor=extract_json_double(json_content, "DI_factor"),
        Voc_stc=extract_json_double(json_content, "Voc (V)"),
        Isc_stc=extract_json_double(json_content, "Isc (A)"),
        Tk_Voc=0.0,
        Tk_Isc=0.0
    )
    
    # Temperature coefficients (convert from %/°C to decimal)
    tk_voc_pct = extract_json_double(json_content, "Tk Pmax (%/°C)")
    if tk_voc_pct == 0.0:
        tk_voc_pct = extract_json_double(json_content, "Tk Voc (%/°C)")
    params.Tk_Voc = (tk_voc_pct / 100.0) if tk_voc_pct != 0.0 else -0.25 / 100.0
    
    tk_isc_pct = extract_json_double(json_content, "Tk Isc (%/°C)")
    params.Tk_Isc = (tk_isc_pct / 100.0) if tk_isc_pct != 0.0 else 0.04 / 100.0
    
    # If parameters are missing, estimate them from available data
    try:
        if params.IL <= 0 or params.I0 <= 0 or params.Rsh <= 0:
            estimate_single_diode_params(params)
    except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError) as e:
        print(f"Error estimating parameters: {e}")
        # Try to set defaults
        if params.IL <= 0:
            params.IL = params.Isc_stc * 1.01 if params.Isc_stc > 0 else 1.0
        if params.I0 <= 0:
            params.I0 = 1e-12
        if params.Rsh <= 0:
            params.Rsh = 200.0
        if params.DI_factor <= 0:
            params.DI_factor = 1.3
    
    # Validate required parameters
    if params.IL <= 0 or params.I0 <= 0 or params.Rsh <= 0:
        print(f"Error: Invalid model parameters")
        print(f"  IL={params.IL}, I0={params.I0}, Rsh={params.Rsh}")
        return False, None, ""
    
    return True, params, part_number


def normalize_curve(curve: List[IVPoint], voc: float, isc: float) -> List[Tuple[float, float]]:
    """Normalize IV curve: V' = V/Voc, I' = I/Isc"""
    normalized = []
    for point in curve:
        V_norm = (point.V / voc) if voc > 0 else 0.0
        I_norm = (point.I / isc) if isc > 0 else 0.0
        normalized.append((V_norm, I_norm))
    return normalized


def process_pv_panel(json_path: str, db: PVPerformanceDatabase) -> bool:
    """Process a single PV panel JSON file."""
    try:
        success, params, part_number = parse_pv_json(json_path)
        if not success:
            return False
        
        print(f"Processing: {part_number}")
    except Exception as e:
        print(f"Error parsing JSON file {json_path}: {e}")
        return False
    
    # Grid definition
    # Skip G=0 since zero irradiance produces no voltage/current
    G_min = 100
    G_max = 1000
    G_step = 100
    T_min = 0
    T_max = 60
    T_step = 5
    
    total_points = 0
    success_count = 0
    
    # Calculate total points for progress tracking
    total_grid_points = len(range(G_min, G_max + 1, G_step)) * len(range(T_min, T_max + 1, T_step))
    processed_count = 0
    
    # Iterate through grid
    for G in range(G_min, G_max + 1, G_step):
        for T in range(T_min, T_max + 1, T_step):
            total_points += 1
            processed_count += 1
            
            try:
                # Calculate IV curve
                curve = SingleDiodeModel.calculate_iv_curve(params, float(G), float(T), 30)
                
                if not curve:
                    continue  # Skip silently for empty curves
                
                # Get Voc and Isc
                voc, isc = SingleDiodeModel.calculate_voc_isc(params, float(G), float(T))
                
                # Validate results
                if voc <= 0 or isc <= 0 or not (0 < voc < 1000) or not (0 < isc < 100):
                    continue  # Skip silently for invalid values
                
                # Normalize curve
                normalized = normalize_curve(curve, voc, isc)
                
                if not normalized:
                    continue  # Skip silently for empty normalized curves
                
                # Insert into database
                if db.insert_performance_point(part_number, G, T, voc, isc, normalized):
                    success_count += 1
                else:
                    print(f"Warning: Failed to insert G={G}, T={T}")
            
            except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError) as e:
                # Skip math errors silently - they're expected for some edge cases
                continue
            except Exception as e:
                # Only print unexpected errors
                print(f"Unexpected error for G={G}, T={T}: {type(e).__name__}: {e}")
                continue
            
            # Progress indicator every 10 points
            if processed_count % 10 == 0:
                print(f"  Progress: {processed_count}/{total_grid_points} points processed...", end='\r')
    
    print(f"\n  Processed {success_count}/{total_points} grid points")
    return success_count > 0


def main():
    """Main function."""
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <database_path> <pv_system_directory>")
        print(f"Example: {sys.argv[0]} pv_performance.db ../pv_panel")
        return 1
    
    db_path = sys.argv[1]
    pv_dir = sys.argv[2]
    
    # Initialize database
    db = PVPerformanceDatabase()
    if not db.initialize(db_path):
        print("Error: Failed to initialize database")
        return 1
    
    print(f"Database initialized: {db_path}")
    
    # Process all JSON files in directory
    processed = 0
    failed = 0
    
    try:
        pv_path = Path(pv_dir)
        json_files = [f for f in pv_path.glob("*.json") if f.name != "topologies_options.json"]
        total_files = len(json_files)
        
        print(f"Found {total_files} PV panel files to process\n")
        
        for idx, json_file in enumerate(json_files, 1):
            print(f"[{idx}/{total_files}] ", end="")
            try:
                if process_pv_panel(str(json_file), db):
                    processed += 1
                else:
                    failed += 1
                    print(f"Failed to process: {json_file.name}")
            except (OverflowError, ValueError, ZeroDivisionError, ArithmeticError) as e:
                failed += 1
                print(f"Math error processing {json_file.name}: {e}")
            except Exception as e:
                failed += 1
                print(f"Unexpected error processing {json_file.name}: {type(e).__name__}: {e}")
    
    except Exception as e:
        print(f"Error: {e}")
        return 1
    
    print(f"\nSummary:")
    print(f"  Processed: {processed} panels")
    print(f"  Failed: {failed} panels")
    
    db.close()
    return 1 if failed > 0 else 0


if __name__ == "__main__":
    sys.exit(main())

