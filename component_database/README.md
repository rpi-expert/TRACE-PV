# Component Database System

This folder contains the database system for managing component parameters for reliability models and PV panel performance data.

## Structure

```
component_database/
├── capacitor/                 # Capacitor JSON files
├── fan_cooling/               # Fan cooling JSON files
├── power_module/              # Power module JSON files
├── pcb/                       # PCB JSON files
├── pv_inverter/               # Inverter JSON files
├── grid/                      # Grid JSON files
├── pv_panel/                  # PV panel JSON files
├── offline_trainning/         # Pure-Python PV data generator
├── initialize_all.py          # Unified initializer and verifier
├── init_database.py           # Component schema and JSON loaders
├── verify_database.py         # Independent database verification
├── component_database.h       # C++ header for database interface
├── component_database.cpp     # C++ implementation for database interface
└── component_parameters.db    # Unified SQLite database
```

## Database Schema

The database contains two types of data:
1. **Component Parameters**: Reliability model parameters for capacitors, fans, power modules, and PCBs
2. **PV Performance Maps**: Pre-calculated IV curves for PV panels at various irradiance and temperature conditions

## Usage

### 1. Initialize Database

Run the unified initialization script to create the database, load all JSON
files, generate PV performance data, and verify the result:

```bash
cd component_database
python3 initialize_all.py
```

The command skips data that is already complete. After changing component or
PV JSON files, rebuild the database with:

```bash
python3 initialize_all.py --force
```

This will:
- Create SQLite database `component_parameters.db`
- Create tables for each component type
- Create PV performance maps table
- Load all JSON files from the component folders
- Generate PV performance data with the pure-Python offline generator
- Return a non-zero exit code if initialization or verification fails

### 2. Initialize PV Performance Table (if needed separately)

If the database already exists and you want to add the PV performance table:

```bash
cd component_database
python3 init_pv_performance_table.py
```

This creates the `pv_performance_maps` table for storing pre-calculated IV curves.

### 3. Populate PV Performance Database

Normally `initialize_all.py` populates this table automatically. To run only
the pure-Python PV generator for troubleshooting:

```bash
cd component_database
python3 offline_trainning/offline_data_generator.py \
  component_parameters.db pv_panel
```

This will:
- Read PV panel JSON files from `pv_panel/`
- Generate IV curves for a grid of conditions (irradiance: 0-1000 W/m², temperature: 0-60°C)
- Store the results in the database

### 4. Verify Database

Check that all JSON files are in the database:

```bash
python3 verify_database.py
```

This will:
- Scan all JSON files in component folders
- Check if they exist in the database
- Report any missing files
- Check PV panel JSON files and their database status

### 5. Use in C++ Code

Before simulation starts, load component parameters:

```cpp
#include "component_database/component_database.h"

// Initialize database
ComponentDatabase db;
db.initialize("component_database/component_parameters.db");

// Load parameters from input file
db.preload_all_components("component_database/example_input_parameters.json");

// Get parameters during simulation (no database access needed)
const CapacitorCoefficients* cap_coeffs = db.get_capacitor("CAP001");
CapacitorType cap_type = db.get_capacitor_type("CAP001");

const FanCoefficients* fan_coeffs = db.get_fan_cooling("FAN001");
const PowerModuleCoefficients* igbt_coeffs = db.get_power_module("IGBT001");

const PCBDimensions* pcb_dims = db.get_pcb_dimensions("PCB001");
const SolderJointCoefficients* pcb_coeffs = db.get_pcb_coefficients("PCB001");
```

### Using PV Performance Database

The PV performance data is accessed through the `pv_performance_db` interface:

```cpp
#include "simulation_preparations/offline_trainning/pv_performance_db.h"

// Initialize PV performance database
PVPerformanceDatabase pv_db;
pv_db.initialize("component_database/component_parameters.db");

// Load performance grid for a specific panel
std::vector<PVGridPoint> grid_points;
pv_db.load_grid("CHSM66M-HC (210)", grid_points);

// Use with IV curve simulator
#include "simulation_preparations/offline_trainning/iv_curve_simulator.h"
IVCurveSimulator simulator;
simulator.initialize("component_database/component_parameters.db", "CHSM66M-HC (210)");
double current = simulator.get_current(650.0, 25.0, 30.0);
```

## PV Performance Database

The PV performance database stores pre-calculated IV curves for faster simulation:

- **Table**: `pv_performance_maps`
- **Schema**: PartNumber, Irradiance, Temperature, Voc, Isc, ShapeData (BLOB)
- **Purpose**: Store normalized IV curve data for bilinear interpolation
- **Population**: Generated by `offline_trainning/offline_data_generator.py`

See `offline_trainning/README.md` for detailed documentation.

## Input Parameter File Format

The input parameter file is a JSON file listing part numbers for each component type:

```json
{
  "capacitor": ["CAP001", "CAP002"],
  "fan_cooling": ["FAN001", "FAN002"],
  "power_module": ["IGBT001", "IGBT002"],
  "pcb": ["PCB001", "PCB002"]
}
```

## Adding New Components

1. Create a JSON file in the appropriate component folder (for example, `capacitor/NEW_PART.json`).
2. Run `python3 initialize_all.py --force` to rebuild the unified database.
3. Run `python3 verify_database.py` for an independent verification pass.

## JSON File Formats

### Capacitor
```json
{
  "part_number": "CAP001",
  "capacitor_type": "ALUMINUM_ELECTROLYTIC",
  "coefficients": {
    "A": 1e-6,
    "B": 2.0,
    "C": 1.5,
    "D": 1.2,
    "Ea": 0.7,
    "V_rated": 400.0,
    "T_ref": 298.15,
    "RH_ref": 50.0
  },
  "description": "Description here"
}
```

### Fan Cooling
```json
{
  "part_number": "FAN001",
  "coefficients": {
    "A": 1e-5,
    "B": 1.8,
    "C": 1.0,
    "Ea": 0.65,
    "T_ref": 298.15,
    "RH_ref": 50.0
  },
  "description": "Description here"
}
```

### Power Module
```json
{
  "part_number": "IGBT001",
  "coefficients": {
    "A": 1e5,
    "B": 2.0,
    "C": 0.5,
    "D": -2.0,
    "Ea": 0.8,
    "T_ref": 298.15
  },
  "description": "Description here"
}
```

### PCB
```json
{
  "part_number": "PCB001",
  "dimensions": {
    "length": 100.0,
    "width": 80.0,
    "thickness": 1.6
  },
  "solder_joint_coefficients": {
    "A": 1e4,
    "B": 1.5,
    "C": 0.3,
    "D": -1.8,
    "Ea": 0.6,
    "T_ref": 298.15
  },
  "description": "Description here"
}
```

## Dependencies

- Python 3.x (for scripts)
- SQLite3 (Python sqlite3 module, included in standard library)
- SQLite3 C library (for C++ code, typically `libsqlite3-dev` on Linux)
- C++ compiler with C++11 or later

## Building C++ Code

Add to your Makefile or build system:

```makefile
# Add SQLite3 library
LIBS += -lsqlite3

# Add component_database source
SOURCES += component_database/component_database.cpp
```
