#!/usr/bin/env python3
"""
Verification script to ensure all JSON files are in the database.
This script scans JSON files and checks if they exist in the database.
"""

import sqlite3
import json
import os
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
    "grid": "grid",
}

# PV system folder
PV_SYSTEM_FOLDER = "pv_panel"

def verify_component(conn, component_type, json_dir):
    """Verify all JSON files for a component type are in database."""
    cursor = conn.cursor()
    table_name = component_type
    
    # Get all part numbers from database
    cursor.execute(f"SELECT part_number, json_file FROM {table_name}")
    db_parts = {row[0]: row[1] for row in cursor.fetchall()}
    
    # Get all JSON files
    json_files = list(json_dir.glob("*.json"))
    json_parts = {}
    
    for json_file in json_files:
        try:
            with open(json_file, 'r') as f:
                data = json.load(f)
                part_number = data.get('part_number')
                if component_type == "grid" and not part_number:
                    part_number = json_file.stem
                if part_number:
                    json_parts[part_number] = json_file.name
        except Exception as e:
            print(f"  Error reading {json_file.name}: {e}")
    
    # Check for missing files
    missing_in_db = []
    missing_in_files = []
    
    for part_number, json_file in json_parts.items():
        if part_number not in db_parts:
            missing_in_db.append((part_number, json_file))
        elif db_parts[part_number] != json_file:
            print(f"  Warning: {part_number} in DB has different filename: {db_parts[part_number]} vs {json_file}")
    
    for part_number, json_file in db_parts.items():
        if part_number not in json_parts:
            missing_in_files.append((part_number, json_file))
    
    return {
        'total_in_db': len(db_parts),
        'total_in_files': len(json_parts),
        'missing_in_db': missing_in_db,
        'missing_in_files': missing_in_files
    }

def verify_pv_panels(conn, json_dir):
    """Verify PV panel JSON files exist (for reference)."""
    json_files = list(json_dir.glob("*.json"))
    pv_files = []
    
    for json_file in json_files:
        # Skip topology options file
        if json_file.name == "topologies_options.json":
            continue
            
        try:
            with open(json_file, 'r') as f:
                data = json.load(f)
                if 'PVpanel' in data:
                    pv_data = data['PVpanel']
                    part_number = pv_data.get('Part Number', '')
                    if part_number:
                        pv_files.append((part_number, json_file.name))
        except Exception as e:
            print(f"  Error reading {json_file.name}: {e}")
    
    # Check if PV performance table exists and has data
    cursor = conn.cursor()
    cursor.execute("""
        SELECT name FROM sqlite_master 
        WHERE type='table' AND name='pv_performance_maps'
    """)
    table_exists = cursor.fetchone() is not None
    
    db_panels = set()
    if table_exists:
        cursor.execute("SELECT DISTINCT PartNumber FROM pv_performance_maps")
        db_panels = {row[0] for row in cursor.fetchall()}
    
    json_panels = {pn for pn, _ in pv_files}
    
    missing_in_db = [(pn, fn) for pn, fn in pv_files if pn not in db_panels]
    in_db_not_in_files = [pn for pn in db_panels if pn not in json_panels]
    
    return {
        'table_exists': table_exists,
        'total_in_db': len(db_panels),
        'total_in_files': len(json_panels),
        'missing_in_db': missing_in_db,
        'in_db_not_in_files': in_db_not_in_files
    }

def main():
    """Main function to verify database."""
    script_dir = Path(__file__).parent.absolute()
    db_path = script_dir / DB_FILE
    
    if not db_path.exists():
        print(f"Error: Database {DB_FILE} does not exist.")
        print("Please run initialize_all.py first.")
        return 1
    
    # Connect to database
    conn = sqlite3.connect(str(db_path))
    
    try:
        print("Verifying component database...")
        print("=" * 60)
        
        all_ok = True
        
        for component_type, folder in COMPONENT_FOLDERS.items():
            json_dir = script_dir / folder
            
            if not json_dir.exists():
                print(f"\n{component_type.upper()}: Directory not found: {json_dir}")
                all_ok = False
                continue
            
            print(f"\n{component_type.upper()}:")
            result = verify_component(conn, component_type, json_dir)
            
            print(f"  Total in database: {result['total_in_db']}")
            print(f"  Total in JSON files: {result['total_in_files']}")
            
            if result['missing_in_db']:
                print(f"  MISSING IN DATABASE ({len(result['missing_in_db'])}):")
                for part_number, json_file in result['missing_in_db']:
                    print(f"    - {part_number} ({json_file})")
                all_ok = False
            
            if result['missing_in_files']:
                print(f"  IN DATABASE BUT NOT IN FILES ({len(result['missing_in_files'])}):")
                for part_number, json_file in result['missing_in_files']:
                    print(f"    - {part_number} ({json_file})")
                all_ok = False
            
            if not result['missing_in_db'] and not result['missing_in_files']:
                print(f"  ✓ All files verified!")
        
        # Verify PV panels
        pv_dir = script_dir / PV_SYSTEM_FOLDER
        if pv_dir.exists():
            print(f"\nPV SYSTEM:")
            pv_result = verify_pv_panels(conn, pv_dir)
            
            if not pv_result['table_exists']:
                print(f"  WARNING: PV performance table does not exist.")
                print(f"  Run initialize_all.py to create and populate it.")
                all_ok = False
            else:
                print(f"  Total panels in database: {pv_result['total_in_db']}")
                print(f"  Total panel JSON files: {pv_result['total_in_files']}")
                
                if pv_result['missing_in_db']:
                    print(f"  NOTE: {len(pv_result['missing_in_db'])} panel JSON files found but no data in database.")
                    print(f"  These panels need to be processed by initialize_all.py.")
                    print(f"  First 5: {', '.join([pn for pn, _ in pv_result['missing_in_db'][:5]])}")
                    all_ok = False
                
                if pv_result['in_db_not_in_files']:
                    print(f"  WARNING: {len(pv_result['in_db_not_in_files'])} panels in DB but JSON files not found:")
                    for pn in list(pv_result['in_db_not_in_files'])[:5]:
                        print(f"    - {pn}")
                    all_ok = False
        else:
            print(f"\nPV SYSTEM: Directory not found: {pv_dir}")
            all_ok = False
        
        print("\n" + "=" * 60)
        if all_ok:
            print("VERIFICATION PASSED: All component JSON files are in the database.")
            return 0
        else:
            print("VERIFICATION FAILED: Some component files are missing.")
            print("Please run initialize_all.py --force to rebuild the database.")
            return 1
        
    finally:
        conn.close()

if __name__ == "__main__":
    exit(main())
