# PV Performance Database System - Implementation Summary

## Overview

This implementation provides a complete hybrid simulation architecture for accelerating IV curve generation in PV system simulations. The system uses a pre-calculated lookup database with bilinear interpolation to replace expensive runtime calculations.

**Note**: The database is now managed in `component_database/`. The PV performance table is stored in the same database file as component parameters. See `component_database/README.md` for database initialization instructions.

## Architecture Components

### 1. Database Schema (`pv_performance_db.h/cpp`)

**Schema:**
```sql
CREATE TABLE pv_performance_maps (
    PartNumber TEXT NOT NULL,
    Irradiance INTEGER NOT NULL,
    Temperature INTEGER NOT NULL,
    Voc REAL NOT NULL,
    Isc REAL NOT NULL,
    ShapeData BLOB NOT NULL,
    PRIMARY KEY (PartNumber, Irradiance, Temperature)
```

**Key Features:**
- Composite primary key: (PartNumber, Irradiance, Temperature)
- Binary serialization of normalized IV curves
- Efficient lookup with index on PartNumber

### 2. Single Diode Model (`single_diode_model.h/cpp`)

**Model Equation:**
```
I = IL - I0*(exp((V+I*Rs)/(n*Vt)) - 1) - (V+I*Rs)/Rsh
```

**Implementation:**
- Newton-Raphson iterative solver for current calculation
- Temperature and irradiance parameter adjustment
- Generates full IV curves with configurable point density
- Handles edge cases (Voc, Isc calculation)

### 3. Offline Data Generator (`offline_data_generator.cpp`)

**Purpose:** Pre-populate database with IV curves for all panels

**Grid Definition:**
- Irradiance: 0 to 1000 W/m² (Step: 100) → 11 points
- Temperature: 0 to 60 °C (Step: 5) → 13 points
- Total: 143 grid points per panel

**Process:**
1. Reads PV panel JSON files from `component_database/pv_system/` directory
2. Extracts Single Diode Model parameters
3. Generates IV curve for each grid point
4. Normalizes curves (V' = V/Voc, I' = I/Isc)
5. Stores in unified database (`component_database/component_parameters.db`)

**Usage:**
```bash
# Using the merged component database location (from offline_trainning directory):
./offline_data_generator ../../../component_database/component_parameters.db ../../../component_database/pv_system
```

### 4. Runtime Simulator (`iv_curve_simulator.h/cpp`)

**Key Optimization:**
- Loads entire grid for a PartNumber into memory once
- No database queries during simulation
- Fast bilinear interpolation for any (T, G) condition

**Bilinear Interpolation Algorithm:**

For a point (G, T) between grid points:
1. Find 4 surrounding points: (G₀,T₀), (G₁,T₀), (G₀,T₁), (G₁,T₁)
2. Calculate interpolation weights:
   - w_G = (G - G₀) / (G₁ - G₀)
   - w_T = (T - T₀) / (T₁ - T₀)
3. Interpolate Voc, Isc, and normalized shape data
4. Denormalize to get actual IV curve

**Usage:**
```cpp
IVCurveSimulator simulator;
simulator.initialize("../../../component_database/component_parameters.db", "CHSM66M-HC (210)");

// Get current at specific voltage
double I = simulator.get_current(650.0, 25.0, 30.0);

// Get full IV curve
std::vector<double> V, I;
simulator.get_iv_curve(650.0, 25.0, V, I);
```

### 5. Case Study Example (`case_study_example.cpp`)

**Demonstrates:** Complete interpolation process for G=650 W/m² from G=600 and G=700

**Mathematical Walkthrough:**
1. Interpolate Voc: `Voc = Voc_600 × (1-w_G) + Voc_700 × w_G`
2. Interpolate Isc: `Isc = Isc_600 × (1-w_G) + Isc_700 × w_G`
3. Normalize voltage: `V_norm = V / Voc`
4. Interpolate normalized shape: `I_norm = I_norm_600 × (1-w_G) + I_norm_700 × w_G`
5. Denormalize: `I = I_norm × Isc`

## File Structure

```
TRACE-PV/
├── component_database/
│   ├── component_parameters.db       # Unified database (components + PV performance)
│   ├── init_database.py              # Database initialization
│   ├── init_pv_performance_table.py  # PV table initialization
│   ├── verify_database.py            # Database verification
│   └── pv_system/                    # PV panel JSON files
│
├── sqlite3/                          # SQLite3 source installation
│   ├── include/sqlite3.h
│   └── lib/libsqlite3.so*
│
└── src/simulation_preparations/offline_trainning/
    ├── README.md                     # User documentation
    ├── IMPLEMENTATION_SUMMARY.md     # This file
    ├── Makefile                      # Build configuration
    ├── pv_performance_db.h/cpp       # Database interface
    ├── single_diode_model.h/cpp      # Single Diode Model solver
    ├── offline_data_generator.cpp    # Database populator
    ├── iv_curve_simulator.h/cpp      # Runtime simulator
    ├── case_study_example.cpp        # Detailed case study
    └── usage_example.cpp             # Simple usage example
```

## Build Instructions

### Prerequisites
- C++17 compiler (g++ or clang++)
- SQLite3 (installed from source in `TRACE-PV/sqlite3/`)
- Standard C++ library

### Database Setup

Before building, ensure the database is initialized:

```bash
cd component_database
python3 init_database.py
```

This creates the unified database with component and PV performance tables.

### Build
```bash
cd src/simulation_preparations/offline_trainning
make
```

The Makefile automatically detects SQLite3 in `TRACE-PV/sqlite3/` and uses it for compilation.

This creates:
- `offline_data_generator` - Database populator
- `case_study_example` - Case study demonstration
- `usage_example` - Simple usage example

## Usage Workflow

### Step 1: Initialize Database
```bash
cd component_database
python3 init_database.py
```
This creates the unified database with all necessary tables (components and PV performance).

### Step 2: Generate PV Performance Data
```bash
cd src/simulation_preparations/offline_trainning
make
./offline_data_generator ../../../component_database/component_parameters.db ../../../component_database/pv_system
```
This processes all JSON files in `component_database/pv_system/` and populates the database with IV curves.

### Step 3: Use in Simulation
```cpp
#include "simulation_preparations/offline_trainning/iv_curve_simulator.h"

// Initialize once (use absolute or relative path from your working directory)
IVCurveSimulator simulator;
simulator.initialize("component_database/component_parameters.db", part_number);  // From project root
// OR:
// simulator.initialize("../../../component_database/component_parameters.db", part_number);  // From offline_trainning

// Use in simulation loop
for (const auto& point : mission_profile) {
    double I = simulator.get_current(
        point.irradiance,
        point.temperature,
        requested_voltage
    );
}
```

### Step 4: Verify (Optional)
```bash
# From offline_trainning directory:
./case_study_example ../../../component_database/component_parameters.db "CHSM66M-HC (210)"
```

### Step 5: Verify Database (Optional)
```bash
cd component_database
python3 verify_database.py
```
This checks that all component and PV panel files are properly loaded in the database.

## Performance Characteristics

1. **Pre-calculation:** IV curves computed once offline (minutes)
2. **Memory-based lookup:** Grid loaded once per PartNumber (milliseconds)
3. **Fast interpolation:** O(1) grid lookup + O(n) interpolation where n = curve points
4. **Scalability:** Handles thousands of mission profile points efficiently

## Mathematical Details

### Single Diode Model Parameters

From JSON file:
- `IL`: Light-generated current at STC
- `I0`: Diode saturation current at STC
- `Rs`: Series resistance
- `Rsh`: Shunt resistance
- `DI_factor`: Diode ideality factor
- `Tk_Voc`, `Tk_Isc`: Temperature coefficients

### Parameter Adjustment

**Light Current:**
```
IL_adj = IL_stc × (G/G_ref) × (1 + Tk_Isc × (T - T_ref))
```

**Diode Saturation Current:**
```
I0_adj = I0_stc × (T/T_ref)³ × exp((Eg×q/k) × (1/T_ref - 1/T))
```

Where:
- Eg = 1.12 eV (silicon bandgap)
- q = 1.602×10⁻¹⁹ C (elementary charge)
- k = 1.381×10⁻²³ J/K (Boltzmann constant)
- Eg×q/k ≈ 13000 K

### Bilinear Interpolation

For a point (G, T) with surrounding points:
- (G₀, T₀), (G₁, T₀), (G₀, T₁), (G₁, T₁)

**Weights:**
```
w_G = (G - G₀) / (G₁ - G₀)
w_T = (T - T₀) / (T₁ - T₀)
```

**Interpolation:**
```
f(G,T) = f(G₀,T₀) × (1-w_G) × (1-w_T) +
         f(G₁,T₀) × w_G × (1-w_T) +
         f(G₀,T₁) × (1-w_G) × w_T +
         f(G₁,T₁) × w_G × w_T
```

## Testing

### Case Study: G=650 Interpolation

**Input:**
- Grid points: G=600, G=700 (both at T=25°C)
- Request: G=650 W/m², T=25°C, V=30V

**Process:**
1. w_G = (650-600)/(700-600) = 0.5
2. Voc_interp = Voc_600 × 0.5 + Voc_700 × 0.5
3. Isc_interp = Isc_600 × 0.5 + Isc_700 × 0.5
4. V_norm = 30 / Voc_interp
5. I_norm = interpolate from normalized curves
6. I = I_norm × Isc_interp

**Verification:**
Run `case_study_example` to see detailed step-by-step calculation.

## Integration Notes

- **Unified Database**: PV performance data is stored in the same database as component parameters (`component_database/component_parameters.db`)
- **Database Management**: Database initialization and management scripts are in `component_database/`
- **C++ Code Location**: All C++ code remains in `src/simulation_preparations/offline_trainning/`
- **SQLite3 Installation**: SQLite3 is installed from source in `TRACE-PV/sqlite3/` (project-local)
- The system is designed to be integrated into the TRACE-PV simulator
- Database can be pre-populated for all panels in the system
- Simulator loads grid data once per PartNumber at initialization
- No database I/O during simulation loop
- Thread-safe for single-threaded use (SQLite handles concurrency)

## Future Enhancements

1. **Caching:** Add LRU cache for frequently accessed curves
2. **Parallel generation:** Multi-threaded offline data generation
3. **Compression:** Compress ShapeData blob for smaller database
4. **Validation:** Add unit tests for interpolation accuracy
5. **Extrapolation:** Handle points outside grid bounds

