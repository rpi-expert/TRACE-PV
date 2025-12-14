#!/usr/bin/env python3
"""
Database initialization script for component parameters.
This script creates SQLite database tables and populates them from JSON files.
Run this script only the first time to establish the database.
"""

import sqlite3
import json
import os
import sys
from pathlib import Path

# Database file path
DB_FILE = "component_parameters.db"

# Component folders
COMPONENT_FOLDERS = {
    "capacitor": "capacitor",
    "fan_cooling": "fan_cooling",
    "power_module": "power_module",
    "pcb": "pcb",
    "pv_inverter": "pv_inverter",
    "grid": "grid"
}

def create_tables(conn):
    """Create database tables for each component type."""
    cursor = conn.cursor()
    
    # Drop existing capacitor table if it exists
    cursor.execute("DROP TABLE IF EXISTS capacitor")
    
    # Capacitor table - supports both FILM and ALUMINUM_ELECTROLYTIC types
    # FILM: A, n, Ea, beta, V_rated, T_ref, RH_ref
    # ALUMINUM_ELECTROLYTIC: L0, V0, T0, beta_min, beta_max
    # Thermal and electrical parameters: rth_amb, rth_surf, esr
    cursor.execute("""
        CREATE TABLE capacitor (
            part_number TEXT PRIMARY KEY,
            capacitor_type TEXT NOT NULL,
            voltage_type TEXT NOT NULL,
            -- FILM capacitor parameters
            A REAL,
            n REAL,
            Ea REAL,
            beta REAL,
            V_rated REAL,
            T_ref REAL,
            RH_ref REAL,
            -- ALUMINUM_ELECTROLYTIC capacitor parameters
            L0 REAL,
            V0 REAL,
            T0 REAL,
            beta_min REAL,
            beta_max REAL,
            -- Thermal and electrical parameters
            rth_amb REAL,
            rth_surf REAL,
            esr REAL,
            capacitance REAL,
            description TEXT,
            json_file TEXT NOT NULL
        )
    """)
    
    # Drop existing fan_cooling table if it exists
    cursor.execute("DROP TABLE IF EXISTS fan_cooling")
    
    # Fan cooling table - supports separate electrical and mechanical coefficients
    # Electrical: A, n, Ea
    # Mechanical: A, c
    cursor.execute("""
        CREATE TABLE fan_cooling (
            part_number TEXT PRIMARY KEY,
            -- Electrical coefficients
            A_electrical REAL,
            n REAL,
            Ea REAL,
            -- Mechanical coefficients
            A_mechanical REAL,
            c REAL,
            description TEXT,
            json_file TEXT NOT NULL
        )
    """)
    
    # Power module table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS power_module (
            part_number TEXT PRIMARY KEY,
            deltaT_A REAL NOT NULL,
            deltaT_n REAL NOT NULL,
            deltaT_Ea REAL NOT NULL,
            arrhenius_A REAL NOT NULL,
            arrhenius_n1 REAL NOT NULL,
            arrhenius_n2 REAL NOT NULL,
            arrhenius_Ea REAL NOT NULL,
            arrhenius_RH_ref REAL NOT NULL,
            arrhenius_T_ref REAL NOT NULL,
            arrhenius_V_ref REAL NOT NULL,
            description TEXT,
            json_file TEXT NOT NULL
        )
    """)
    
    # PCB table - updated to match new JSON structure
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS pcb (
            part_number TEXT PRIMARY KEY,
            component_type TEXT,
            material TEXT,
            -- Component dimensions
            component_length REAL,
            component_width REAL,
            component_thickness REAL,
            -- Copper dimensions
            copper_length REAL,
            copper_width REAL,
            copper_thickness REAL,
            -- Solder joint dimensions
            solder_length REAL,
            solder_width REAL,
            solder_thickness REAL,
            -- Ductility coefficients
            ductility_A REAL,
            ductility_B REAL,
            ductility_C REAL,
            ductility_D REAL,
            ductility_E REAL,
            -- Material properties
            shear_modulus REAL,
            CTE_component REAL,
            G_copper REAL,
            G_FR4 REAL,
            CTE_FR4 REAL,
            E_FR4 REAL,
            Poisson_FR4 REAL,
            pcb_thickness REAL,
            -- Thermal cycling parameters
            max_temperature REAL,
            min_temperature REAL,
            dwell_time REAL,
            ramp_time REAL,
            -- Lifetime parameters
            beta REAL,
            lifetime_N63 REAL,
            adjust_param REAL,
            description TEXT,
            json_file TEXT NOT NULL
        )
    """)
    
    # PV Inverter table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS pv_inverter (
            part_number TEXT PRIMARY KEY,
            boost_included INTEGER NOT NULL,
            model_stage INTEGER NOT NULL,
            topology_level INTEGER NOT NULL,
            modulation_scheme TEXT NOT NULL,
            switching_frequency REAL NOT NULL,
            Vdc REAL NOT NULL,
            CDC1 REAL,
            CDC2 REAL,
            L1 REAL NOT NULL,
            R1 REAL NOT NULL,
            C1 REAL NOT NULL,
            L2 REAL NOT NULL,
            R2 REAL NOT NULL,
            C2 REAL NOT NULL,
            Lboost REAL,
            RLboost REAL,
            Cboost REAL,
            boost_frequency REAL,
            description TEXT,
            json_file TEXT NOT NULL
        )
    """)
    
    # Grid table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS grid (
            part_number TEXT PRIMARY KEY,
            grid_voltage REAL NOT NULL,
            grid_frequency REAL NOT NULL,
            grid_phase REAL NOT NULL,
            reference_phase_magnitude REAL NOT NULL,
            reference_frequency REAL NOT NULL,
            reference_phase_shift REAL NOT NULL,
            description TEXT,
            json_file TEXT NOT NULL
        )
    """)
    
    # PV Performance Maps table (for pre-calculated IV curves)
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS pv_performance_maps (
            PartNumber TEXT NOT NULL,
            Irradiance INTEGER NOT NULL,
            Temperature INTEGER NOT NULL,
            Voc REAL NOT NULL,
            Isc REAL NOT NULL,
            ShapeData BLOB NOT NULL,
            PRIMARY KEY (PartNumber, Irradiance, Temperature)
        )
    """)
    
    cursor.execute("""
        CREATE INDEX IF NOT EXISTS idx_partnumber 
        ON pv_performance_maps(PartNumber)
    """)
    
    conn.commit()
    print("Database tables created successfully.")

def load_capacitor(conn, json_file_path):
    """Load capacitor data from JSON file."""
    with open(json_file_path, 'r') as f:
        data = json.load(f)
    
    cursor = conn.cursor()
    coeffs = data['coefficients']
    cap_type = data['capacitor_type']
    voltage_type = data.get('voltage_type', 'DC')  # Default to DC if not specified
    
    # Get thermal and electrical parameters (common for all capacitor types)
    rth_amb = data.get('rth_amb', 0.5)  # Default if not specified
    rth_surf = data.get('rth_surf', 0.3)  # Default if not specified
    esr = data.get('esr', 0.01)  # Default if not specified
    capacitance = data.get('capacitance', 330e-6)  # Default if not specified (330 uF)
    
    if cap_type == "FILM":
        # Film capacitor: A, n, Ea, beta, V_rated, T_ref, RH_ref
        cursor.execute("""
            INSERT OR REPLACE INTO capacitor 
                (part_number, capacitor_type, voltage_type, A, n, Ea, beta, V_rated, T_ref, RH_ref, rth_amb, rth_surf, esr, capacitance, description, json_file)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            data['part_number'],
            cap_type,
            voltage_type,
            coeffs.get('A'),
            coeffs.get('n'),
            coeffs.get('Ea'),
            coeffs.get('beta'),
            coeffs.get('V_rated'),
            coeffs.get('T_ref'),
            coeffs.get('RH_ref'),
            rth_amb,
            rth_surf,
            esr,
            capacitance,
            data.get('description', ''),
            os.path.basename(json_file_path)
        ))
    elif cap_type == "ALUMINUM_ELECTROLYTIC":
        # Aluminum electrolytic: L0, V0, T0, beta_min, beta_max
        cursor.execute("""
            INSERT OR REPLACE INTO capacitor 
            (part_number, capacitor_type, voltage_type, L0, V0, T0, beta_min, beta_max, rth_amb, rth_surf, esr, capacitance, description, json_file)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            data['part_number'],
            cap_type,
            voltage_type,
            coeffs.get('L0'),
            coeffs.get('V0'),
            coeffs.get('T0'),
            coeffs.get('beta_min'),
            coeffs.get('beta_max'),
            rth_amb,
            rth_surf,
            esr,
            capacitance,
            data.get('description', ''),
            os.path.basename(json_file_path)
        ))
    else:
        raise ValueError(f"Unknown capacitor type: {cap_type}")
    
    conn.commit()
    return data['part_number']

def load_fan_cooling(conn, json_file_path):
    """Load fan cooling data from JSON file."""
    with open(json_file_path, 'r') as f:
        data = json.load(f)
    
    cursor = conn.cursor()
    electrical_coeffs = data.get('electrical_coefficients', {})
    mechanical_coeffs = data.get('mechanical_coefficients', {})
    
    cursor.execute("""
        INSERT OR REPLACE INTO fan_cooling 
        (part_number, A_electrical, n, Ea, A_mechanical, c, description, json_file)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        data['part_number'],
        electrical_coeffs.get('A'),
        electrical_coeffs.get('n'),
        electrical_coeffs.get('Ea'),
        mechanical_coeffs.get('A'),
        mechanical_coeffs.get('c'),
        data.get('description', ''),
        os.path.basename(json_file_path)
    ))
    
    conn.commit()
    return data['part_number']

def load_power_module(conn, json_file_path):
    """Load power module data from JSON file."""
    with open(json_file_path, 'r') as f:
        data = json.load(f)
    
    cursor = conn.cursor()
    coeffs = data['coefficients']
    
    # Extract deltaT model coefficients
    deltaT_coeffs = coeffs.get('deltaT_model', {})
    # Extract Arrhenius model coefficients
    arrhenius_coeffs = coeffs.get('arrhenius_model', {})
    
    cursor.execute("""
        INSERT OR REPLACE INTO power_module 
        (part_number, deltaT_A, deltaT_n, deltaT_Ea, arrhenius_A, arrhenius_n1, arrhenius_n2, arrhenius_Ea, arrhenius_RH_ref, arrhenius_T_ref, arrhenius_V_ref, description, json_file)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        data['part_number'],
        deltaT_coeffs.get('A', 3e4),
        deltaT_coeffs.get('n', -3.22),
        deltaT_coeffs.get('Ea', 0.042),
        arrhenius_coeffs.get('A', 259.6930),
        arrhenius_coeffs.get('n1', 2.66),
        arrhenius_coeffs.get('n2', 2.2),
        arrhenius_coeffs.get('Ea', 0.79),
        arrhenius_coeffs.get('RH_ref', 95.0),
        arrhenius_coeffs.get('T_ref', 348.15),
        arrhenius_coeffs.get('V_ref', 800.0),
        data.get('description', ''),
        os.path.basename(json_file_path)
    ))
    
    conn.commit()
    return data['part_number']

def load_pcb(conn, json_file_path):
    """Load PCB data from JSON file."""
    with open(json_file_path, 'r') as f:
        data = json.load(f)
    
    cursor = conn.cursor()
    
    # Extract nested structures
    component_dims = data.get('component_dimensions', {})
    copper_dims = data.get('copper_dimensions', {})
    solder_dims = data.get('solder_joint_dimensions', {})
    ductility = data.get('ductility_coefficient', {})
    
    # Handle component_dimensions that might have length_min/width_min/thickness_min or length/width/thickness
    component_l = component_dims.get('length_min', component_dims.get('length', 0.0))
    component_w = component_dims.get('width_min', component_dims.get('width', 0.0))
    component_h = component_dims.get('thickness_min', component_dims.get('thickness', 0.0))
    
    cursor.execute("""
        INSERT OR REPLACE INTO pcb 
        (part_number, component_type, material,
         component_length, component_width, component_thickness,
         copper_length, copper_width, copper_thickness,
         solder_length, solder_width, solder_thickness,
         ductility_A, ductility_B, ductility_C, ductility_D, ductility_E,
         shear_modulus, CTE_component, G_copper, G_FR4, CTE_FR4, E_FR4, Poisson_FR4,
         pcb_thickness, max_temperature, min_temperature, dwell_time, ramp_time,
         beta, lifetime_N63, adjust_param, description, json_file)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        data['part_number'],
        data.get('component_type', ''),
        data.get('material', ''),
        component_l,
        component_w,
        component_h,
        copper_dims.get('length', 0.0),
        copper_dims.get('width', 0.0),
        copper_dims.get('thickness', 0.0),
        solder_dims.get('length', 0.0),
        solder_dims.get('width', 0.0),
        solder_dims.get('thickness', 0.0),
        ductility.get('A', 0.0),
        ductility.get('B', 0.0),
        ductility.get('C', 0.0),
        ductility.get('D', 0.0),
        ductility.get('E', 0.0),
        data.get('shear_modulus', 50000.0),
        data.get('CTE_component_para', 7.1e-6),
        data.get('G_cooper', 44117.0),
        data.get('G_FR4', 7200.0),
        data.get('CTE_FR4', 0.00001755066),
        data.get('C_FR4', 17000.0),
        data.get('Poisson_FR4', 0.18),
        data.get('pcb_thickness', 1.57),
        data.get('max_temperature', 145.0),
        data.get('min_temperature', 60.0),
        data.get('dewll_time', data.get('dwell_time', 5.0)),
        data.get('ramp_time', 7.0),
        data.get('beta', 3.82),
        data.get('lifetime@N63', 3710.0),
        data.get('adjust_param', 113.5596),
        data.get('description', ''),
        os.path.basename(json_file_path)
    ))
    
    conn.commit()
    return data['part_number']

def load_pv_inverter(conn, json_file_path):
    """Load PV inverter data from JSON file."""
    with open(json_file_path, 'r') as f:
        data = json.load(f)
    
    cursor = conn.cursor()
    
    # Extract nested structures
    inverter_params = data.get('inverter_related_params', {})
    boost_params = data.get('boost_related_params', {})
    
    # Convert boost_included to integer (True/False -> 1/0)
    boost_included = 1 if data.get('boost_included', False) else 0
    
    cursor.execute("""
        INSERT OR REPLACE INTO pv_inverter 
        (part_number, boost_included, model_stage, topology_level, modulation_scheme,
         switching_frequency, Vdc, CDC1, CDC2,
         L1, R1, C1, L2, R2, C2,
         Lboost, RLboost, Cboost, boost_frequency,
         description, json_file)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        data['part_number'],
        boost_included,
        inverter_params.get('model_stage', 1),
        inverter_params.get('topology_level', 2),
        inverter_params.get('modulation_scheme', 'SVM'),
        inverter_params.get('switching_frequency', 16000.0),
        inverter_params.get('Vdc', 1000.0),
        inverter_params.get('CDC1'),
        inverter_params.get('CDC2'),
        inverter_params.get('L1', 0.0003),
        inverter_params.get('R1', 0.03),
        inverter_params.get('C1', 0.00009786),
        inverter_params.get('L2', 0.00093183),
        inverter_params.get('R2', 0.0053),
        inverter_params.get('C2', 0.00009786),
        boost_params.get('Lboost'),
        boost_params.get('RLboost'),
        boost_params.get('Cboost'),
        boost_params.get('boost_frequency'),
        data.get('description', ''),
        os.path.basename(json_file_path)
    ))
    
    conn.commit()
    return data['part_number']

def load_grid(conn, json_file_path):
    """Load grid data from JSON file."""
    with open(json_file_path, 'r') as f:
        data = json.load(f)
    
    cursor = conn.cursor()
    
    # Grid JSON doesn't have part_number, so we'll use filename or generate one
    part_number = data.get('part_number', os.path.splitext(os.path.basename(json_file_path))[0])
    
    cursor.execute("""
        INSERT OR REPLACE INTO grid 
        (part_number, grid_voltage, grid_frequency, grid_phase,
         reference_phase_magnitude, reference_frequency, reference_phase_shift,
         description, json_file)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        part_number,
        data.get('grid_voltage', 277.13),
        data.get('grid_frequency', 60.0),
        data.get('grid_phase', 0.0),
        data.get('reference_phase_magnitude', 404.07),
        data.get('reference_frequency', 60.0),
        data.get('reference_phase_shift', 0.1974),
        data.get('description', ''),
        os.path.basename(json_file_path)
    ))
    
    conn.commit()
    return part_number

def main():
    """Main function to initialize database."""
    script_dir = Path(__file__).parent.absolute()
    db_path = script_dir / DB_FILE
    
    # Check if database already exists
    if db_path.exists():
        response = input(f"Database {DB_FILE} already exists. Recreate? (y/N): ")
        if response.lower() != 'y':
            print("Aborted.")
            return
        db_path.unlink()
    
    # Connect to database
    conn = sqlite3.connect(str(db_path))
    
    try:
        # Create tables
        create_tables(conn)
        
        # Load JSON files
        loaders = {
            "capacitor": load_capacitor,
            "fan_cooling": load_fan_cooling,
            "power_module": load_power_module,
            "pcb": load_pcb,
            "pv_inverter": load_pv_inverter,
            "grid": load_grid
        }
        
        total_loaded = 0
        
        for component_type, folder in COMPONENT_FOLDERS.items():
            json_dir = script_dir / folder
            if not json_dir.exists():
                print(f"Warning: Directory {json_dir} does not exist. Skipping.")
                continue
            
            loader = loaders[component_type]
            json_files = list(json_dir.glob("*.json"))
            
            print(f"\nLoading {component_type} components...")
            for json_file in json_files:
                try:
                    part_number = loader(conn, json_file)
                    print(f"  Loaded: {part_number} from {json_file.name}")
                    total_loaded += 1
                except Exception as e:
                    print(f"  Error loading {json_file.name}: {e}")
        
        print(f"\nDatabase initialization complete!")
        print(f"Total components loaded: {total_loaded}")
        print(f"Database file: {db_path}")
        
    finally:
        conn.close()

if __name__ == "__main__":
    main()

