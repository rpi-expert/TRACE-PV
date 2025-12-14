#!/usr/bin/env python3
"""
Initialize PV performance table in the component database.
This creates the table schema for storing pre-calculated IV curves.
The actual data is populated by the C++ offline_data_generator tool.
"""

import sqlite3
from pathlib import Path

# Database file path
DB_FILE = "component_parameters.db"

def create_pv_performance_table(conn):
    """Create PV performance maps table if it doesn't exist."""
    cursor = conn.cursor()
    
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
    print("PV performance table created successfully.")

def main():
    """Main function to initialize PV performance table."""
    script_dir = Path(__file__).parent.absolute()
    db_path = script_dir / DB_FILE
    
    if not db_path.exists():
        print(f"Error: Database {DB_FILE} does not exist.")
        print("Please run init_database.py first to create the database.")
        return 1
    
    # Connect to database
    conn = sqlite3.connect(str(db_path))
    
    try:
        print(f"Initializing PV performance table in {DB_FILE}...")
        create_pv_performance_table(conn)
        print("Done.")
        return 0
        
    except Exception as e:
        print(f"Error: {e}")
        return 1
    finally:
        conn.close()

if __name__ == "__main__":
    exit(main())

