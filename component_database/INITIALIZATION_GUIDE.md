# Database Initialization Guide

This guide explains how to use the merged initialization script to set up the complete component and PV performance database.

## Quick Start

```bash
cd component_database
python3 initialize_all.py
```

This single command will:
1. Check if database exists (skip if valid)
2. Initialize/create all database tables
3. Load all component JSON files (capacitor, fan, PCB, power module)
4. Run offline training for all PV panels
5. Insert PV performance data into database

## What It Does

### 1. Database Initialization (Skip if Exists)
- Creates database file if it doesn't exist
- Creates all required tables:
  - `capacitor` - Capacitor component parameters
  - `fan_cooling` - Fan cooling component parameters
  - `power_module` - Power module component parameters
  - `pcb` - PCB component parameters
  - `pv_performance_maps` - PV panel IV curve data

### 2. Component Loading
Automatically loads all JSON files from:
- `capacitor/*.json`
- `fan_cooling/*.json`
- `power_module/*.json`
- `pcb/*.json`

### 3. PV Panel Offline Training
- Builds `offline_data_generator` if needed
- Processes all JSON files from `pv_system/*.json`
- Generates IV curves for grid conditions:
  - Irradiance: 0-1000 W/m² (step: 100)
  - Temperature: 0-60°C (step: 5)
- Stores normalized IV curve data in database

## Input File Format

Create an input file (e.g., `example_input_parameters.json`) with component part numbers:

```json
{
  "capacitor": ["CAP001", "CAP002"],
  "fan_cooling": ["FAN001", "FAN002"],
  "power_module": ["IGBT001", "IGBT002"],
  "pcb": ["PCB001", "PCB002"],
  "pv_panel": ["CHSM66M-HC (210)", "FS-6470-P", "JKM615N-78HL4"]
}
```

## Usage in Simulation Code

The main simulation code (`src/main.cpp`) now includes a section that:

1. Loads component part numbers from input file
2. Initializes IV curve simulators for each PV panel
3. Loads component parameters (capacitor, fan, power module, PCB)
4. Processes mission profile points with IV curve lookups

Example usage in code:
```cpp
// IV curves are automatically loaded based on:
// - Mission profile (irradiance, temperature from CSV)
// - PV panel part numbers from input file

// Access IV curve data:
double current = iv_simulator->get_current(
    mission_profile.irradiance,
    mission_profile.temperature,
    requested_voltage
);
```

## Verification

After initialization, verify everything:

```bash
cd component_database
python3 verify_database.py
```

This will check:
- All component JSON files are in database
- PV panel JSON files exist
- PV performance data is populated

## Troubleshooting

### Database Already Exists
- Script will skip initialization if database is valid
- PV training will still run if data is missing

### Offline Training Fails
- Make sure `offline_trainning/offline_data_generator` is built:
  ```bash
  cd offline_trainning
  make
  ```

### Missing Components
- Add JSON files to appropriate folders
- Re-run `initialize_all.py` (it updates existing entries)

### Missing PV Panels
- Add JSON files to `pv_system/` folder
- Re-run initialization (will only process new panels)

## File Structure

```
component_database/
├── initialize_all.py          # Merged initialization script
├── example_input_parameters.json  # Example input file
├── component_parameters.db     # Unified database
├── capacitor/                  # Capacitor JSON files
├── fan_cooling/               # Fan cooling JSON files
├── power_module/              # Power module JSON files
├── pcb/                       # PCB JSON files
├── pv_system/                 # PV panel JSON files
└── offline_trainning/         # PV offline training tools
    ├── offline_data_generator
    └── ...
```

