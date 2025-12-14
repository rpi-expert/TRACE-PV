#!/usr/bin/env python3
"""
Merged initialization script for component database.
This script:
1. Initializes database (skips if already exists and verified)
2. Inserts all available components (capacitor, fan, PCB, power module)
3. Performs offline training for PV panels and inserts results to database
"""

import sqlite3
import json
import os
import sys
import subprocess
from pathlib import Path

# Database file path
DB_FILE = "component_parameters.db"

# Component folders
COMPONENT_FOLDERS = {
    "capacitor": "capacitor",
    "fan_cooling": "fan_cooling",
    "power_module": "power_module",
    "pcb": "pcb"
}

def check_database_exists_and_valid(conn):
    """Check if database exists and has all required tables."""
    cursor = conn.cursor()
    
    # Check for component tables
    required_tables = ["capacitor", "fan_cooling", "power_module", "pcb", "pv_performance_maps"]
    cursor.execute("""
        SELECT name FROM sqlite_master 
        WHERE type='table' AND name IN (?, ?, ?, ?, ?)
    """, tuple(required_tables))
    
    existing_tables = {row[0] for row in cursor.fetchall()}
    missing_tables = set(required_tables) - existing_tables
    
    return len(missing_tables) == 0, missing_tables

def create_tables(conn):
    """Create database tables for each component type and PV performance."""
    cursor = conn.cursor()
    
    # Capacitor table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS capacitor (
            part_number TEXT PRIMARY KEY,
            capacitor_type TEXT NOT NULL,
            A REAL NOT NULL,
            B REAL NOT NULL,
            C REAL NOT NULL,
            D REAL NOT NULL,
            Ea REAL NOT NULL,
            V_rated REAL NOT NULL,
            T_ref REAL NOT NULL,
            RH_ref REAL NOT NULL,
            description TEXT,
            json_file TEXT NOT NULL
        )
    """)
    
    # Fan cooling table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS fan_cooling (
            part_number TEXT PRIMARY KEY,
            A REAL NOT NULL,
            B REAL NOT NULL,
            C REAL NOT NULL,
            Ea REAL NOT NULL,
            T_ref REAL NOT NULL,
            RH_ref REAL NOT NULL,
            description TEXT,
            json_file TEXT NOT NULL
        )
    """)
    
    # Power module table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS power_module (
            part_number TEXT PRIMARY KEY,
            A REAL NOT NULL,
            B REAL NOT NULL,
            C REAL NOT NULL,
            D REAL NOT NULL,
            Ea REAL NOT NULL,
            T_ref REAL NOT NULL,
            description TEXT,
            json_file TEXT NOT NULL
        )
    """)
    
    # PCB table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS pcb (
            part_number TEXT PRIMARY KEY,
            length REAL NOT NULL,
            width REAL NOT NULL,
            thickness REAL NOT NULL,
            A REAL NOT NULL,
            B REAL NOT NULL,
            C REAL NOT NULL,
            D REAL NOT NULL,
            Ea REAL NOT NULL,
            T_ref REAL NOT NULL,
            description TEXT,
            json_file TEXT NOT NULL
        )
    """)
    
    # PV Performance Maps table
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
    print("Database tables created/verified successfully.")

def load_capacitor(conn, json_file_path):
    """Load capacitor data from JSON file."""
    with open(json_file_path, 'r') as f:
        data = json.load(f)
    
    cursor = conn.cursor()
    coeffs = data['coefficients']
    
    cursor.execute("""
        INSERT OR REPLACE INTO capacitor 
        (part_number, capacitor_type, A, B, C, D, Ea, V_rated, T_ref, RH_ref, description, json_file)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        data['part_number'],
        data['capacitor_type'],
        coeffs['A'],
        coeffs['B'],
        coeffs['C'],
        coeffs['D'],
        coeffs['Ea'],
        coeffs['V_rated'],
        coeffs['T_ref'],
        coeffs['RH_ref'],
        data.get('description', ''),
        os.path.basename(json_file_path)
    ))
    
    conn.commit()
    return data['part_number']

def load_fan_cooling(conn, json_file_path):
    """Load fan cooling data from JSON file."""
    with open(json_file_path, 'r') as f:
        data = json.load(f)
    
    cursor = conn.cursor()
    coeffs = data['coefficients']
    
    cursor.execute("""
        INSERT OR REPLACE INTO fan_cooling 
        (part_number, A, B, C, Ea, T_ref, RH_ref, description, json_file)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        data['part_number'],
        coeffs['A'],
        coeffs['B'],
        coeffs['C'],
        coeffs['Ea'],
        coeffs['T_ref'],
        coeffs['RH_ref'],
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
    
    cursor.execute("""
        INSERT OR REPLACE INTO power_module 
        (part_number, A, B, C, D, Ea, T_ref, description, json_file)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        data['part_number'],
        coeffs['A'],
        coeffs['B'],
        coeffs['C'],
        coeffs['D'],
        coeffs['Ea'],
        coeffs['T_ref'],
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
    dims = data['dimensions']
    coeffs = data['solder_joint_coefficients']
    
    cursor.execute("""
        INSERT OR REPLACE INTO pcb 
        (part_number, length, width, thickness, A, B, C, D, Ea, T_ref, description, json_file)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        data['part_number'],
        dims['length'],
        dims['width'],
        dims['thickness'],
        coeffs['A'],
        coeffs['B'],
        coeffs['C'],
        coeffs['D'],
        coeffs['Ea'],
        coeffs['T_ref'],
        data.get('description', ''),
        os.path.basename(json_file_path)
    ))
    
    conn.commit()
    return data['part_number']

def run_offline_training(script_dir):
    """Run offline training for PV panels."""
    offline_trainning_dir = script_dir / "offline_trainning"
    offline_data_generator = offline_trainning_dir / "offline_data_generator"
    
    if not offline_data_generator.exists():
        print(f"\nWarning: offline_data_generator not found at {offline_data_generator}")
        print("  Please build it first: cd offline_trainning && make")
        return False
    
    db_path = script_dir / DB_FILE
    pv_system_dir = script_dir / "pv_panel"
    
    print(f"\nRunning offline training for PV panels...")
    print(f"  Database: {db_path}")
    print(f"  PV system directory: {pv_system_dir}")
    
    try:
        # Change to offline_trainning directory to run the generator
        result = subprocess.run(
            [str(offline_data_generator), str(db_path), str(pv_system_dir)],
            cwd=str(offline_trainning_dir),
            capture_output=True,
            text=True,
            check=True
        )
        print(result.stdout)
        if result.stderr:
            print("Warnings:", result.stderr)
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error running offline training: {e}")
        print(f"stdout: {e.stdout}")
        print(f"stderr: {e.stderr}")
        return False
    except FileNotFoundError:
        print(f"Error: Could not execute {offline_data_generator}")
        print("  Make sure it's built and executable")
        return False

def main():
    """Main function to initialize everything."""
    script_dir = Path(__file__).parent.absolute()
    db_path = script_dir / DB_FILE
    
    # Connect to database (create if doesn't exist)
    conn = sqlite3.connect(str(db_path))
    
    try:
        # Check if database is already initialized
        is_valid, missing_tables = check_database_exists_and_valid(conn)
        
        if is_valid:
            print(f"Database {DB_FILE} already exists and appears to be complete.")
            print("Skipping initialization. Use --force to reinitialize.")
            
            # Check if PV performance data exists
            cursor = conn.cursor()
            cursor.execute("SELECT COUNT(*) FROM pv_performance_maps")
            pv_count = cursor.fetchone()[0]
            
            if pv_count == 0:
                print("\nPV performance data is missing. Running offline training...")
                run_offline_training(script_dir)
            else:
                print(f"PV performance data exists ({pv_count} entries). Skipping offline training.")
            
            conn.close()
            return 0
        
        # Create tables
        print("Creating database tables...")
        create_tables(conn)
        
        # Load component JSON files
        loaders = {
            "capacitor": load_capacitor,
            "fan_cooling": load_fan_cooling,
            "power_module": load_power_module,
            "pcb": load_pcb
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
        
        print(f"\nComponent database initialization complete!")
        print(f"Total components loaded: {total_loaded}")
        
        # Run offline training for PV panels
        if not run_offline_training(script_dir):
            print("\nWarning: Offline training failed. PV performance data may be incomplete.")
        
        print(f"\nDatabase initialization complete!")
        print(f"Database file: {db_path}")
        
        return 0
        
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        return 1
    finally:
        conn.close()

if __name__ == "__main__":
    exit(main())

