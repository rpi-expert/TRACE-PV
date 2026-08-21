# Database Merge Summary

This document summarizes the merge of PV performance database management into the component_database system.

## Changes Made

### 1. Database Schema Integration

- **Added PV performance table** to `init_database.py`:
  - Creates `pv_performance_maps` table in the same database as component parameters
  - Table schema matches the original design from offline_trainning
  
- **Created separate initialization script** (`init_pv_performance_table.py`):
  - Allows adding PV performance table to existing databases
  - Useful when database already exists and only PV table is needed

### 2. Verification Updates

- **Updated `verify_database.py`**:
  - Added PV panel JSON file verification
  - Checks if PV performance table exists
  - Reports panels that need to be processed by offline_data_generator

### 3. Documentation Updates

- **Updated `component_database/README.md`**:
  - Added PV performance database documentation
  - Included usage examples for PV database initialization
  - Added section on using PV performance data in C++

- **Updated `offline_trainning/README.md`**:
  - Added note about merged database location
  - Updated examples to use component_database paths
  - Added database setup section

### 4. Build System Updates

- **Updated `Makefile`**:
  - Improved SQLite3 header detection
  - Added warning for missing SQLite3 development headers
  - Updated example usage to reflect merged database location

### 5. File Structure

The merged structure:
```
component_database/
├── component_parameters.db          # Unified database (components + PV performance)
├── init_database.py                 # Creates all tables including PV performance
├── init_pv_performance_table.py     # Separate script for PV table only
├── verify_database.py               # Verifies components and PV panels
├── pv_system/                       # PV panel JSON files
└── [component folders]/             # Component JSON files

src/simulation_preparations/offline_trainning/
├── pv_performance_db.h/cpp          # C++ database interface (unchanged)
├── offline_data_generator.cpp       # Populates PV performance data
├── iv_curve_simulator.h/cpp         # Runtime simulator (unchanged)
└── [other C++ files]                # Unchanged
```

## Usage Workflow

1. **Initialize Database**:
   ```bash
   cd component_database
   python3 init_database.py
   ```

2. **Generate PV Performance Data**:
   ```bash
   cd src/simulation_preparations/offline_trainning
   make
   ./offline_data_generator ../../component_database/component_parameters.db ../../component_database/pv_system
   ```

3. **Verify Database**:
   ```bash
   cd component_database
   python3 verify_database.py
   ```

## Key Benefits

- **Unified Database**: All component and PV data in one database file
- **Consistent Management**: Follows same pattern as component database
- **Backward Compatible**: C++ code remains unchanged, only paths updated
- **Better Organization**: Clear separation between database management (Python) and usage (C++)

## Notes

- The C++ code (`pv_performance_db.h/cpp`, `iv_curve_simulator.h/cpp`) remains in `offline_trainning/` as requested
- Database creation/management is now in `component_database/` following the existing pattern
- The PV performance table is in the same database file as component parameters for easier management
- All paths in examples and documentation have been updated to reflect the new structure

